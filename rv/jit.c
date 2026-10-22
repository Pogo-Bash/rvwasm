/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The JIT: hot RISC-V code is translated to WebAssembly.
 *
 * The unit of translation is a region: starting from a block that has run
 * often, the translator follows the static control flow (branches, jumps,
 * fall-throughs) and collects up to JIT_MAX_BLOCKS basic blocks. A region
 * becomes one Wasm function:
 *
 *   load the guest registers the region uses into locals
 *   loop
 *     leave if a signal is pending or the instruction budget is used up
 *     br_table to the block selected by $next
 *     block k: its instructions, then either
 *              $pc = target, $next = its index, continue the loop, or
 *              $pc = target, leave  (the target is outside the region)
 *              (an indirect jump looks its target up in a small table
 *              of the region's blocks to tell which, see gen_insn())
 *   store the registers the region writes, $pc and the instruction count
 *   if it left for an address that some region has a block for: jump to that
 *   region (a tail call), handing it what remains of the budget
 *
 * Registers stay in locals for as long as execution stays inside the region,
 * and every block of a region is an entry point (the function takes the
 * index of the block to start with).
 *
 * The interpreter remains the reference and the fallback. Translated code
 * leaves, with $pc at the instruction in question and nothing of it executed,
 * whenever
 *   - a memory access fails its inline check (bounds, page protection, page
 *     crossing): the interpreter repeats it and raises the precise fault;
 *   - the next instruction is one the JIT does not translate (system calls,
 *     atomics, the counter CSRs, anything unusual).
 *
 * Floating point: the f registers and fcsr live in *cpu throughout, never
 * in locals. Loads and stores of them are inline. Arithmetic, comparisons
 * and conversions are the host's instructions under the conditions that make
 * the host's result and flags the interpreter's (fp.c says which, and why),
 * tested inline; when they do not hold, and for the instructions that are
 * not worth it, it is a call to the interpreter's own code (rv_jit_fp()).
 * Either way the results and the exception flags are exactly the
 * interpreter's.
 *
 * Code that changes. A program may make code while it runs, on pages it can
 * write to, and change it there (a JavaScript engine does little else).
 * Translated code must never be run stale, and the rules for that are:
 *
 *   - A page that code is translated from is marked first (RV_PAGE_CODE),
 *     and a marked page does not allow writing as it stands, whatever its
 *     mapping allows (code_page()). Everything that writes guest memory,
 *     translated code and system calls included, looks at what a page allows
 *     as it stands; so a write to a marked page goes the slow way, where mm.c
 *     takes the mark off, lets the page be written again and says which page
 *     it was (rv_jit_code_changed()), all before the write. The same is said
 *     when such a page is unmapped, gets other memory or may no longer be
 *     executed. (A change of protection that leaves it executable changes
 *     nothing here: the mark stays, and so do the translations.)
 *   - What is said is: these pages. Every thread then suspends the regions
 *     it has made from them: they leave the map, so nothing reaches them,
 *     and are kept, with a copy of the code they were made from. When
 *     execution next comes to a block of a suspended region, its pages are
 *     marked again and the code there is compared with the copy: the same,
 *     and the region is in use again as it is, which costs next to nothing
 *     (a program that puts new code next to old code on a page, or takes
 *     the right to execute away while it writes and gives it back, loses no
 *     translation); different, and the region is discarded and the code is
 *     translated again when it has run often enough.
 *   - riscv_flush_icache says the same for the pages of its range that are
 *     marked. That covers the writes that cannot be seen as they are made: one
 *     whose address a thread already had when another thread marked the
 *     page, the host's for a system call, one through a second mapping of
 *     the same memory. As on a real machine, the program has to flush when
 *     one thread changes code that another may be running, and a thread
 *     that runs code while another changes it may run either; but once the
 *     flush has returned, no thread runs a translation of the old code:
 *     whoever says that pages changed also asks every other thread to leave
 *     translated code at the end of the block it is in (RV_SIG_CODE in its
 *     sig_pending, which every region looks at anyway).
 *   - Code that keeps being changed is translated less and less eagerly
 *     (jit_count.fails), and a region that keeps being suspended and taken up
 *     again without having run much in between is given up (R_STRIKES): a page
 *     that holds both code and data that is written all the time is better
 *     interpreted.
 *
 * Wasm code cannot be generated from inside Wasm: the host compiles the
 * module (__rv_jit_install, see tools/patch-runtime.py) and puts its function
 * into the function table of the instance that asked. Every thread is an
 * instance of its own (thread.c), so every thread has its own translations:
 * its own struct rv_jit, made when the thread has run for a while, and
 * nothing in it is ever touched by another thread. What the threads have in
 * common:
 *   - the marks on pages that code was translated from (RV_PAGE_CODE);
 *   - rv_proc.code_epoch and code_log: the thread that changes such pages
 *     notes which and counts up, and every thread looks at what it has not
 *     seen yet before it runs any translation again (catch_up()). When the
 *     page table moves, whose address is in the code, it is everything, and
 *     that is discarded for good;
 *   - the buffers a translation is put together in, one translation at a
 *     time.
 * A forked child starts without translations.
 *
 * RV_JIT_FAKE=1, on a host that cannot compile Wasm (the native build): all of
 * the above is done, regions are made and kept and suspended, and where a
 * region would run the interpreter runs its block instead, after a look
 * whether the code the region was made from is still what is there. If not,
 * a stale translation would have run: that is reported and ends the program.
 * For testing the rules above with real programs, and under the host's tools.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "rv.h"
#include "wasm_emit.h"
#include "wasm_emu.h"

#define JIT_MAX_BLOCKS	128
/*
 * Regions are made one at a time, each a module of its own, and every module
 * takes at least a page of the engine's code space (16 KiB on a Mac) where
 * most regions' machine code is a few KiB: once enough of them wait, the
 * functions of those still there are compiled again as one module (batch_make()).
 * Which regions make a batch is decided by what they are, not by how many
 * came before or when: only regions next to each other in the guest's code
 * (batch_bucket(): the same file, the same 512 bytes of it) make a batch, and
 * it ends with the first region of that stretch (JIT_BATCH_MIN waiting in it
 * or more) whose function's hash has JIT_BATCH_MASK's bits clear, or after
 * JIT_BATCH_MAX, and its functions are in the order of their hashes. So a
 * program started again, which makes mostly the same regions in another order
 * (from the store, at once, instead of after being run a few hundred times,
 * and in whatever order its processes and threads come), makes mostly the
 * same batches, byte for byte, and the engine (or the page: tools/patch-runtime.py,
 * 28) compiles each, and optimizes its hot functions, only once. Measured on
 * Konsole started three times: 5, 14, 38 percent of the batches' bytes the
 * same as an earlier launch's when a batch ended after 8 or more regions of
 * whatever came; 11, 72, 97 percent with these.
 * (The cost: about 2400 modules of a launch instead of 150, a second more in the
 * first launch. RV_JIT_GROUP=min:mask:shift tries others.)
 */
#define JIT_BATCH_MIN	1
#define JIT_BATCH_MASK	1
#define JIT_BATCH_MAX	96
#define JIT_PENDING_MAX	512	/* regions waiting in all buckets together: past it the oldest region's bucket goes */
#define JIT_GROUP_SHIFT	9	/* 512 bytes of a file's code; 64 or more: no buckets, every region is in the one (RV_JIT_GROUP=8:15:64 is the old way) */
#define JIT_MAX_INSNS	4096	/* per region */
#define JIT_BLOCK_INSNS	256	/* per block */
#define JIT_BUDGET	0x10000	/* guest instructions per call of a region */
/*
 * Interpreted runs of a block before it is translated. Translating a region
 * costs about as much as interpreting tens of thousands of instructions, so
 * a low number makes short programs slower (with 20, a shell loop starting
 * `true` 200 times took 1.8 s instead of 1.5 s with the JIT off) and gains
 * long ones nothing measurable. Between 100 and 1000 it makes no difference.
 */
#define JIT_HOT		200
/*
 * ... of code the program made itself, which is in no file's mapping. Whoever
 * made it had a reason: mostly it is a compiler that found the code worth
 * compiling, or an interpreter that was generated.
 */
#define JIT_HOT_MADE	50
#define JIT_POLL	0x40000	/* guest instructions between signal polls */
#define JIT_WARM	1	/* interpreted runs that get a block into a region: see warm() */
#define JALR_SITES_BITS	9	/* indirect jumps the interpreter remembers the targets of (struct jalr_site) */
#define JALR_TARGETS	16
#define JALR_INLINE	3	/* a jump with at most this many targets seen compares with each inline (gen_insn(), JALR) */

/*
 * The map of translated blocks and the table of counts start small and grow
 * with the program (map_grow()): most processes are small, and each has its
 * own. The counts have as many places as the map. (With a quarter as many,
 * blocks pushed each other out before they had been counted as running, were
 * left out of regions, and sort and awk took a quarter longer.)
 */
#define MAP_BITS_MIN	12
#define MAP_BITS_MAX	20
#define NEVER		0xffffffffu

/* The buffers a translation is assembled in: this large to begin with, larger when a region needs it. */
#define CODE_CAP_MIN	(128u << 10)
#define CODE_CAP_MAX	(4u << 20)

/* Locals of a region function. Parameters first. */
#define L_CPU		0
#define L_ENTRY		1
#define L_BUDGET_IN	2	/* guest instructions it may run before returning to the dispatcher */
#define L_REG(r)	(2 + (r))	/* x1..x31 in locals 3..33 */
#define L_PC		34
#define L_ADDR		35
#define L_TMP		36
#define L_FP		37	/* a floating-point result, as bits, before it is known to be good */
#define L_HOST		38	/* the host address of the access at L_ADDR, once check_access() has passed */
#define L_NEXT		39
#define L_BUDGET	40
#define L_TMP32		41
#define L_CHAIN		42	/* it is leaving for a jump target, which may be another region's */
#define L_MAP		43	/* cpu->map and cpu->npages, read when the region is called: see check_access() */
#define L_NPAGES	44
/*
 * The translation cache (check_access()): for each of the PCACHE_SLOTS guest
 * registers that address memory most in the region, the page its last access
 * was on. L_CB_R(k) and L_CB_W(k) are the guest address of that page, or
 * CACHE_NONE, according to whether it may be read, and written; L_CD(k) is
 * what to add to a guest address in it to get the host address.
 */
#define PCACHE_SLOTS	8
#define L_CB_R(k)	(45 + 3 * (k))
#define L_CB_W(k)	(46 + 3 * (k))
#define L_CD(k)		(47 + 3 * (k))
#define CACHE_NONE	0x8000000000000000ULL	/* a page no address is within 4 KiB of */
#define N_I64_LOCALS	36
#define N_I32_LOCALS	4
#define N_I64_MORE	2	/* L_MAP, L_NPAGES */
/*
 * Only in code made with the access path's parts on (OFF_OUTLINE, OFF_HOIST),
 * a fourth group, i64: the failed access the shared slow block is to finish
 * (see check_access_as()), and the pages that groups of accesses by one base
 * register were checked for (CACHE_SLOTS of them).
 */
#define L_HOW		45	/* size | prot << 8 | aligned << 16 | instructions not run << 32 */
#define L_SPARE		46
#define L_CPAGE(s)	(47 + 2 * (s))	/* the guest address of the page slot s was checked for */
#define L_CDELTA(s)	(48 + 2 * (s))	/* ... and host - guest there */
#define CACHE_SLOTS	4

/*
 * Each region that has an indirect jump has a table of its own blocks for
 * them (mostly returns): entries found by hashing the address, with no
 * second place to look. A block whose entry is taken is simply not in the
 * table, and a jump to it leaves the region like a jump to anywhere else.
 * To keep that rare, each region gets the hash multiplier, out of
 * TARGET_TRIES, that leaves the fewest blocks out, and a table with twice
 * as many entries as it has blocks (at least TARGET_BITS_MIN bits' worth, at
 * most TARGET_BITS'); the multiplier and the size are constants in the
 * region's code, so this costs nothing when it runs. (Every region had 256
 * entries, 4 KiB, once: with each block translated the first time it ran,
 * that was half a gigabyte in cc1plus, most of it empty.)
 */
#define TARGET_BITS	8
#define TARGET_BITS_MIN	2
#define TARGET_TRIES	24
#define TARGET_NONE	(~0ULL)		/* odd, so never a jump target */

struct jit_target {
	uint64_t pc;
	uint64_t index;		/* of the block in its region */
};

/* Functions imported from the emulator itself (namespace "rv"), in this order. */
static const char *const helpers[] = {
	"rv_op_mulh", "rv_op_mulhsu", "rv_op_mulhu", "rv_op_div", "rv_op_divu",
	"rv_op_rem", "rv_op_remu", "rv_op_divw", "rv_op_divuw", "rv_op_remw", "rv_op_remuw",
	"rv_jit_fp", "rv_jit_csr", "rv_jit_fused64", "rv_jit_reserve",
};
enum {
	H_MULH, H_MULHSU, H_MULHU, H_DIV, H_DIVU, H_REM, H_REMU, H_DIVW, H_DIVUW, H_REMW, H_REMUW,
	H_FP, H_CSR,	/* from here on: three arguments, (cpu, insn, a) -> result; before: (a, b) -> result */
	H_FUSED64,	/* (a, b, c) -> a * b + c, all as bits */
	H_RESERVE,	/* (cpu, addr, size | prot << 8 | aligned << 16) -> the host address in a reservation, or 0 */
	N_HELPERS
};
/* The functions a module defines: the region (exported as "r"), and the same as H_RESERVE, faster (gen_reserve_fn()). */
#define F_REGION	N_HELPERS
#define F_RESERVE	(N_HELPERS + 1)
#define F_XLAT		(N_HELPERS + 2)	/* a memory access that missed its translation cache (gen_xlat_fn()) */
#define N_MODULE_FNS	2		/* the functions besides the regions: F_RESERVE, F_XLAT */

typedef void (*rv_region_fn)(struct rv_cpu *, uint32_t entry, uint32_t budget);

#ifdef __wasm__
long __rv_jit_install(const void *code, unsigned long len, long slot);
#endif

/*
 * A place in the map. The address of a block is even. With bit 0 set as
 * well, the block is of a suspended region (see the top): no jump finds it,
 * since no jump goes to an odd address, and the dispatcher, which looks for
 * both, has the region looked over first. 1 with NO_SLOT is a place that was
 * used and is not now: it is not the end of a search.
 */
struct jit_entry {
	uint64_t pc;		/* 0: empty */
	uint32_t slot;		/* function table index of the region */
	uint32_t index;		/* block index inside the region */
};
#define NO_SLOT		0xffffffffu

struct jit_count {
	uint64_t pc;
	uint32_t count;
	uint8_t fails;		/* how often code translated from here was found changed: each time doubles what hot means */
	uint8_t kind;		/* 0: not looked at yet. Or whether the code is in a file's mapping, or made by the program. */
};
#define KIND_FILE	1
#define KIND_MADE	2
#define FAILS_MAX	10
#define FAILS_THRASH	6	/* ... and where a region given up for being suspended all the time starts */

struct jit_insn {
	uint64_t pc;
	uint32_t raw;		/* compressed instructions already expanded */
	uint32_t orig;		/* the bytes as they were read: `len` of them */
	uint8_t len;
};

struct jit_block {
	uint64_t pc;
	uint32_t first, count;	/* instructions, in region.insns */
	uint64_t next_pc;	/* address after the last instruction */
	int is_return;		/* a call in the region returns here */
};

struct layout;

struct jit_region {
	int nblocks, ninsns;
	struct jit_block blocks[JIT_MAX_BLOCKS];
	struct jit_insn insns[JIT_MAX_INSNS];
};

/*
 * A region that exists, by its number: what it was made from. Its blocks
 * (where each starts, how many bytes of code and how many instructions it
 * is) and the code itself as the translator read it, block after block.
 */
struct jit_made {
	uint64_t *pcs;		/* one allocation: this, then the three below */
	uint16_t *sizes, *counts;
	uint8_t *bytes;
	uint32_t nblocks;
	uint8_t state;
	uint8_t strikes;	/* times in a row it was taken up again soon after the last time */
	uint64_t revived;	/* the thread's instruction count when it last was */
	uint32_t kib;		/* what its module takes of the engine's code space, about (code_kib()) */
	uint32_t batch;		/* the batch its function is in now, + 1; 0 while it is in a module of its own */
	uint8_t *body;		/* its function's code, kept until it is put into a batch */
	uint32_t body_len;
	uint32_t *hints, nhints;	/* ... and its branch hints (offsets in it << 1 | likely), as long */
	uint64_t body_hash;	/* of the function's code: the order in a batch, and where a batch ends */
	uint64_t bucket;	/* what the region is next to: only regions of one bucket make a batch (batch_bucket()) */
	uint8_t snapped;	/* RV_JIT_SNAP: the state at its first entry was written out (snapshot()) */
};
enum { R_FREE, R_LIVE, R_SUSPENDED };
#define R_STRIKES	16
#define R_SOON		200000	/* instructions */

struct rv_jit {
	struct jit_entry *map;
	struct jit_count *counts;
	uint32_t map_bits;	/* the map has 1 << map_bits places */
	uint32_t map_used;	/* places that are not empty */
	uint32_t map_dead;	/* ... of which were used and are not now */
	uint32_t hot;
	uint32_t hot_made;	/* for code the program made itself (at most `hot`) */
	uint32_t warm;		/* interpreted runs that make a block worth including in a region */
	/* For hunting a bug in the translator: RV_JIT_OFF=bits turns parts of it off, RV_JIT_LIMIT=n stops after n regions. */
	uint32_t off;
	int tag;		/* RV_JIT_TAG: see tag_table */
	uint64_t limit;
	/* RV_JIT_CHECK: every block translated code runs is compared with the interpreter (see checked_call()). */
	int check;
	uint16_t *map_len;	/* instructions in the block of each map entry, for that */
	uint64_t stat_checked, stat_unchecked;
	uint64_t last_poll;
	uint32_t epoch;		/* rv_proc.code_epoch when these translations were (or began to be) made */

	/*
	 * Regions have numbers. A number has a function table slot, given by
	 * the host when the number is first used and its own from then on; a
	 * number whose region was discarded is used for the next one made.
	 */
	uint32_t *slots;
	uint32_t nslots, slots_cap;
	uint32_t *free_regions;		/* numbers below nslots that have no region now */
	uint32_t nfree;
	uint32_t slot_base;		/* the lowest slot we were given */
	uint32_t *by_slot;		/* region number + 1 by slot - slot_base */
	uint32_t by_slot_cap;
	struct jit_made *made;		/* by number */
	/* The jump target table of each region that has one, freed with the region. */
	struct jit_target **targets;
	/*
	 * Which regions were made from which pages: page << PAGE_REGION_BITS
	 * with the region's number + 1 below, found by hashing the page. A
	 * place that names a region no longer there, or one that was not made
	 * from the page after all, does no harm (a region suspended for
	 * nothing is taken up again when it is next needed), so nothing is
	 * ever taken out: the table is made again from the regions there are
	 * when it is half full.
	 */
	uint64_t *pages;
	uint32_t pages_bits, pages_used;
	int fma;			/* the host multiplies and adds in one rounding: 1, -1 if not, 0 if not asked yet (fma_probe()) */
	int fake;			/* RV_JIT_FAKE: see the top */
	int emu;			/* RV_JIT_EMU: the native build runs translated code by interpreting the Wasm (wasm_emu.c) */
	uint32_t fake_slot;
	struct jit_target *targets_now;	/* of the region being translated (NULL: it has no indirect jump) */
	uint64_t target_multiplier;	/* ... and the hash multiplier chosen for it */
	uint32_t target_bits;		/* ... and its size */

	/* Scratch for the translation in progress: the process's (see scratch_get()), while this thread has it. */
	struct jit_region *region;
	struct layout *layout;
	struct wbuf body, func, module;
	/* OFF_HINTS: the branch hints of the body being made (hint()), offset << 1 | likely; then of the function. */
	uint32_t *hints, nhints, hints_cap;
	uint32_t hint_sites;	/* RV_JIT_HINT_SITES: which of them (HS_*) */
	/*
	 * Where the interpreter saw indirect jumps go (rv_jit_note_jalr()), by
	 * the jump's address: the targets a region's blocks are joined up with
	 * (layout_edges()) and the blocks a region takes in for them
	 * (build_region()). A small table; a site keeps its last JALR_TARGETS
	 * distinct targets.
	 */
	struct jalr_site {
		uint64_t pc;
		uint64_t to[JALR_TARGETS];
		uint8_t n, next;
	} jalr[1u << JALR_SITES_BITS];
	const char *dump_dir;
	const char *snap_dir;	/* RV_JIT_SNAP=DIR: the guest's state at the first entry of each region, for tools/region-bench.js (snapshot()) */
	uint32_t nsnaps, snap_max;
	uint64_t snap_every, snap_next;	/* RV_JIT_SNAP_EVERY=N: instead, at the first region entry every N instructions */
	int names;		/* RV_JIT_NAMES=1: a name section in every module, for the host's profiler (gen_names()) */
	uint32_t batch_min, batch_mask, group_shift;	/* RV_JIT_GROUP=min:mask:shift (see batch_trigger()) */
	uint32_t old;		/* RV_JIT_OLD=bits: as before chromeperf, for measuring against it (OLD_*) */
	int log_fd;		/* RV_JIT_LOG=FILE: a line for every region made (see log_region()), or -1 */
	/*
	 * RV_JIT_FLATMEM=delta (the native build, for tools/region-bench.js
	 * only): memory accesses are not checked at all, the host address is
	 * the guest address plus delta. Wrong for any real use: an experiment
	 * that measures what the page table costs, the harness laying every
	 * page out at that distance.
	 */
	uint64_t flatmem;
	int flatmem_on;
	int pcache_slots;	/* registers with a translation cache per region (RV_JIT_PCACHE, at most PCACHE_SLOTS) */

	uint64_t stat_regions, stat_blocks, stat_insns, stat_bytes, stat_flushes, stat_failed;
	uint64_t stat_nomem;		/* translations, or tidying, given up for want of memory (the interpreter goes on) */
	/* Room for translated code (see there). */
	uint8_t *used;			/* by number: entered since the last look (and see used_pc) */
	uint8_t *used_pc;		/* ... as translated code says it: by a hash of the region's address (used_hash(); cpu->jit_used) */
	uint32_t making;		/* the number of the region being translated */
	uint64_t code_kib;		/* what this thread's modules take, about */
	uint64_t code_cap_kib;		/* ... at most, whatever the budget of all of them */
	int no_room_query;		/* the host has no budget: only code_cap_kib */
	int no_release;			/* the host cannot empty a slot */
	uint64_t last_look;		/* insn_count when the budget was last looked at by the dispatcher */
	uint64_t last_evict_ns;
	uint64_t stat_evictions, stat_evicted, stat_evicted_kib, stat_refused;
	uint64_t room_budget, room_sum;	/* what the host answered last (for RV_JIT_LOG's E lines) */
	uint32_t refuse, room_asked;	/* RV_JIT_REFUSE=n: see room_for() */
	/* Batches (see batch_make()). */
	int batch_off;
	uint32_t pending[JIT_PENDING_MAX];	/* regions in modules of their own, to be put into the next batch of their bucket */
	uint32_t npending;
	uint16_t *batch_live;		/* by batch: its regions still there; 0 once it is gone */
	uint16_t *batch_size;		/* ... and how many it was made with */
	uint32_t nbatches, batches_cap;
	uint64_t stat_batches, stat_batched, stat_batch_failed;
	/* Code that changed: events looked at, regions suspended, taken up again as they were, found changed, given up. */
	uint64_t stat_events, stat_suspended, stat_revived, stat_changed, stat_thrashed, stat_tidied;
	uint64_t stat_same;		/* regions whose module was one made in the process before (module_seen()) */
	/* The store of translations kept outside the process (jitcache.c, "Translations kept" below). */
	int jc;				/* there is a store, and this thread's code may be kept in it */
	int jc_verify;			/* RV_JIT_CACHE_VERIFY=1: what the store gives is made again and compared */
	int hints_off;			/* RV_JIT_HINTS=0: no regions made early from the store's hints */
	const uint64_t *hint;		/* a region being made again as it was made before (hint_translate()): its blocks, sorted */
	uint32_t nhint;
	uint64_t stat_jc_hits, stat_jc_misses, stat_jc_puts, stat_jc_other;
	uint64_t stat_hint_asked, stat_hint_found, stat_hint_made;
	uint64_t stat_ran, stat_calls;	/* guest instructions run by translated code, in how many calls */
	uint64_t lap[4];		/* (with -v or RV_JIT_LOG) ns in the last region's build_region(), store lookup, function, module */
	uint64_t stat_ns_translate, stat_ns_install, stat_ns_start;	/* (with -v) time making regions, time the host took to compile them, when the thread's JIT began */
	/* With -v: where execution went when a region was left. */
	uint64_t stat_to_self, stat_to_self_displaced, stat_to_other, stat_to_interp, stat_budget;
	uint64_t stat_chained;		/* jumps from region to region (counted by code made with -v) */
	uint64_t stat_misses;		/* memory accesses that missed their translation cache (counted by code made with -v) */
	uint64_t stat_opcode[128];	/* ... and which instruction the interpreter was needed for */
	/* With -v: floating-point instructions the translated code left to the call (rv_jit_fp()), by kind: see rv_jit_count_fp(). */
	uint64_t stat_fp_call[72];
};

/* Call the function in table slot `slot` as a region: (cpu, entry, budget). */
static void region_call_raw(struct rv_jit *jit, uint32_t slot, struct rv_cpu *cpu, uint32_t entry, uint32_t budget)
{
#ifdef __wasm__
	(void)jit;
	((rv_region_fn)(uintptr_t)slot)(cpu, entry, budget);
#else
	if (jit->emu && rv_emu_run((long)slot, cpu, entry, budget) < 0) {
		fprintf(stderr, "rv: jit emu: translated code failed at pc 0x%llx\n", (unsigned long long)cpu->pc);
		_exit(97);
	}
#endif
}

/* Run the region in table slot `slot` from its block `entry`, with `budget` instructions. */
static void region_call(struct rv_jit *jit, uint32_t slot, struct rv_cpu *cpu, uint32_t entry, uint32_t budget)
{
	region_call_raw(jit, slot, cpu, entry, budget);
}

/* ------------------------------------------------------------------ lookup */

#define HASH_MULTIPLIER	0x9e3779b97f4a7c15ULL

/*
 * Translated code marks its region as entered by the region's address, not
 * its number (which is the thread's: the code is the same in every thread),
 * in a table of USED_PLACES bytes by a hash of the address. Two regions that
 * share a place are both kept when either was entered; nothing worse.
 */
#define USED_BITS	16
#define USED_PLACES	(1u << USED_BITS)

static uint32_t used_hash(uint64_t pc)
{
	return (uint32_t)(((pc >> 1) * HASH_MULTIPLIER) >> (64 - USED_BITS));
}

/*
 * Translated code looks a jump target up in the map by itself (see the
 * chaining in gen_module()): it compares the pc of the first MAP_PROBES
 * entries the target hashes to with the target and takes a match's word for
 * it. An empty entry has pc 0, and address 0 hashes to entry 0, so the
 * entries looked at for address 0 are kept occupied by an address nothing
 * jumps to: a guest that jumps to 0 must not "find" a region there. The map
 * has MAP_PROBES - 1 spare entries at its end for the same lookup.
 */
#define MAP_PROBES	4
#define MAP_GONE	1	/* with NO_SLOT: used once, or reserved as above */

static void map_reserve(struct jit_entry *map)
{
	for (int i = 0; i < MAP_PROBES; i++) {
		map[i].pc = MAP_GONE;
		map[i].slot = NO_SLOT;
	}
}

static uint32_t hash_pc(uint64_t pc, unsigned bits)
{
	return (uint32_t)(((pc >> 1) * HASH_MULTIPLIER) >> (64 - bits));
}

/* The entry of the block at pc, be its region in use or suspended (then the entry's pc is odd), or NULL. */
static struct jit_entry *map_find(struct rv_jit *jit, uint64_t pc)
{
	uint32_t i = hash_pc(pc, jit->map_bits);

	while (jit->map[i].pc) {
		if ((jit->map[i].pc & ~1ULL) == pc && jit->map[i].slot != NO_SLOT)
			return &jit->map[i];
		i = (i + 1) & ((1u << jit->map_bits) - 1);
	}
	return NULL;
}

/* The block at pc is block `index` of the region in `slot` from now on, whatever it was before. */
static void map_insert(struct rv_jit *jit, uint64_t pc, uint32_t slot, uint32_t index, uint32_t len)
{
	uint32_t i = hash_pc(pc, jit->map_bits), gone = NO_SLOT;

	while (jit->map[i].pc && ((jit->map[i].pc & ~1ULL) != pc || jit->map[i].slot == NO_SLOT)) {
		/* (A place no longer used will do, if the block has none; but not the reserved ones.) */
		if (jit->map[i].slot == NO_SLOT && gone == NO_SLOT && i >= MAP_PROBES)
			gone = i;
		i = (i + 1) & ((1u << jit->map_bits) - 1);
	}
	if (!jit->map[i].pc) {
		if (gone != NO_SLOT) {
			i = gone;
			jit->map_dead--;
		} else {
			jit->map_used++;
		}
	}
	jit->map[i].pc = pc;
	jit->map[i].slot = slot;
	jit->map[i].index = index;
	if (jit->map_len)
		jit->map_len[i] = (uint16_t)len;
}

/* A map and a table of counts with 1 << bits places each, empty. 0, or -1 with nothing changed. */
static int map_alloc(struct rv_jit *jit, uint32_t bits)
{
	struct jit_entry *map = calloc((1u << bits) + MAP_PROBES - 1, sizeof(*map));
	struct jit_count *counts = calloc(1u << bits, sizeof(*counts));
	uint16_t *map_len = jit->check ? calloc(1u << bits, sizeof(*map_len)) : NULL;

	if (!map || !counts || (jit->check && !map_len)) {
		free(map);
		free(counts);
		free(map_len);
		return -1;
	}
	map_reserve(map);
	free(jit->counts);
	free(jit->map_len);
	jit->map = map;
	jit->counts = counts;
	jit->map_len = map_len;
	jit->map_bits = bits;
	jit->map_used = jit->map_dead = 0;
	return 0;
}

/*
 * The map is getting full: one four times the size, with everything in it
 * that the old one had (the blocks of the regions there are, suspended ones
 * too, and the counts of the blocks not translated yet). Translated code
 * finds the map, and its size, through cpu->jit when it chains, so nothing
 * translated has to go. (It had both in it once, and every growth, at 2048
 * blocks, 8192 and 32768, discarded everything: in Chromium's threads that
 * was a good part of all translating.) Returns -1 if it cannot grow.
 */
static int map_grow(struct rv_jit *jit)
{
	uint32_t old_bits = jit->map_bits, bits = old_bits + 2, mask = (1u << bits) - 1, used = 0;
	struct jit_entry *old = jit->map, *map;
	struct jit_count *old_counts = jit->counts, *counts;
	uint16_t *old_len = jit->map_len, *map_len = NULL;

	if (bits > MAP_BITS_MAX)
		return -1;
	map = calloc((1u << bits) + MAP_PROBES - 1, sizeof(*map));
	counts = calloc(1u << bits, sizeof(*counts));
	if (old_len)
		map_len = calloc(1u << bits, sizeof(*map_len));
	if (!map || !counts || (old_len && !map_len)) {
		free(map);
		free(counts);
		free(map_len);
		return -1;
	}
	map_reserve(map);
	for (uint32_t i = 0; i < (1u << old_bits); i++) {
		const struct jit_entry *e = &old[i];
		uint32_t at;

		if (!e->pc || e->slot == NO_SLOT)
			continue;	/* (empty, gone, or one of the reserved places) */
		for (at = hash_pc(e->pc & ~1ULL, bits); map[at].pc; at = (at + 1) & mask)
			;
		map[at] = *e;
		if (map_len)
			map_len[at] = old_len[i];
		used++;
	}
	for (uint32_t i = 0; i < (1u << old_bits); i++)
		if (old_counts[i].pc)
			counts[hash_pc(old_counts[i].pc, bits)] = old_counts[i];
	free(old);
	free(old_counts);
	free(old_len);
	jit->map = map;
	jit->counts = counts;
	jit->map_len = map_len;
	jit->map_bits = bits;
	jit->map_used = used;
	jit->map_dead = 0;
	return 0;
}

/* ----------------------------------------------------------------- regions */

/* The number of the region in a function table slot. */
static uint32_t region_of(const struct rv_jit *jit, uint32_t slot)
{
	return jit->by_slot[slot - jit->slot_base] - 1;
}

/* Room in the tables that go by region number for one more number. 0, or -1. */
static int regions_room(struct rv_jit *jit)
{
	uint32_t cap = jit->slots_cap ? jit->slots_cap * 2 : 256;
	void *p;

	if (jit->nslots < jit->slots_cap)
		return 0;
	if (!(p = realloc(jit->slots, cap * sizeof(*jit->slots))))
		return -1;
	jit->slots = p;
	if (!(p = realloc(jit->free_regions, cap * sizeof(*jit->free_regions))))
		return -1;
	jit->free_regions = p;
	if (!(p = realloc(jit->made, cap * sizeof(*jit->made))))
		return -1;
	jit->made = p;
	memset(jit->made + jit->slots_cap, 0, (cap - jit->slots_cap) * sizeof(*jit->made));
	if (!(p = realloc(jit->targets, cap * sizeof(*jit->targets))))
		return -1;
	jit->targets = p;
	memset(jit->targets + jit->slots_cap, 0, (cap - jit->slots_cap) * sizeof(*jit->targets));
	if (!(p = realloc(jit->used, cap)))
		return -1;
	jit->used = p;
	memset(jit->used + jit->slots_cap, 0, cap - jit->slots_cap);
	jit->slots_cap = cap;
	return 0;
}

/*
 * The host is to let go of the module in a table slot: nothing calls it any
 * more, and the engine can collect it (__rv_jit_install with a length of 0;
 * a host that cannot says -1, and is not asked again).
 */
static void slot_release(struct rv_jit *jit, uint32_t slot)
{
#ifdef __wasm__
	if (!jit->no_release && __rv_jit_install(NULL, 0, slot) < 0)
		jit->no_release = 1;
#else
	if (jit->emu)
		rv_emu_install(NULL, 0, slot);
#endif
}

/* The pages the blocks of a region are on, each once or a few times: `add` is called for every one. */
static void region_pages(struct rv_jit *jit, uint32_t n, void (*add)(struct rv_jit *, uint64_t page, uint32_t n))
{
	const struct jit_made *m = &jit->made[n];
	uint64_t last = ~0ULL;

	for (uint32_t k = 0; k < m->nblocks; k++) {
		uint64_t from = m->pcs[k] >> RV_PAGE_SHIFT, to = (m->pcs[k] + m->sizes[k] - 1) >> RV_PAGE_SHIFT;

		for (uint64_t page = from; page <= to; page++) {
			if (page != last)
				add(jit, page, n);
			last = page;
		}
	}
}

#define PAGE_REGION_BITS 24
#define PAGES_BITS_MIN	8

static uint32_t hash_page(uint64_t page, unsigned bits)
{
	return (uint32_t)((page * HASH_MULTIPLIER) >> (64 - bits));
}

static void pages_add(struct rv_jit *jit, uint64_t page, uint32_t n)
{
	uint64_t word = page << PAGE_REGION_BITS | (n + 1);
	uint32_t i;

	if (!jit->pages)
		return;
	i = hash_page(page, jit->pages_bits);
	while (jit->pages[i]) {
		if (jit->pages[i] == word)
			return;
		i = (i + 1) & ((1u << jit->pages_bits) - 1);
	}
	jit->pages[i] = word;
	jit->pages_used++;
}

/*
 * The table of pages, made again from the regions there are, larger if they
 * need it. Without memory for it there is none, and suspend_pages() looks
 * through the regions themselves.
 */
static void pages_remake(struct rv_jit *jit)
{
	uint32_t bits = jit->pages_bits ? jit->pages_bits : PAGES_BITS_MIN;

	for (;;) {
		free(jit->pages);
		jit->pages_used = 0;
		jit->pages_bits = bits;
		if (!(jit->pages = calloc(1u << bits, sizeof(*jit->pages)))) {
			jit->pages_bits = 0;
			jit->stat_nomem++;
			return;
		}
		for (uint32_t n = 0; n < jit->nslots && jit->pages_used < (1u << bits) / 4; n++)
			if (jit->made[n].state != R_FREE)
				region_pages(jit, n, pages_add);
		if (jit->pages_used < (1u << bits) / 4)
			return;
		bits++;
	}
}

/* Note which pages region n was made from. */
static void pages_note(struct rv_jit *jit, uint32_t n)
{
	if (!jit->pages || jit->pages_used + 2 * jit->made[n].nblocks + 2 > (1u << jit->pages_bits) / 2)
		pages_remake(jit);	/* (which notes it, the region being there) */
	else
		region_pages(jit, n, pages_add);
}

/* Keep what region n was made from: a copy of r's blocks. 0, or -1 without memory. */
static int region_keep(struct rv_jit *jit, uint32_t n, const struct jit_region *r)
{
	struct jit_made *m = &jit->made[n];
	size_t nbytes = 0, at = 0;

	for (int k = 0; k < r->nblocks; k++)
		nbytes += (size_t)(r->blocks[k].next_pc - r->blocks[k].pc);
	if (!(m->pcs = malloc((size_t)r->nblocks * (sizeof(*m->pcs) + sizeof(*m->sizes) + sizeof(*m->counts)) + nbytes)))
		return -1;
	m->sizes = (uint16_t *)(void *)(m->pcs + r->nblocks);
	m->counts = m->sizes + r->nblocks;
	m->bytes = (uint8_t *)(m->counts + r->nblocks);
	m->nblocks = (uint32_t)r->nblocks;
	for (int k = 0; k < r->nblocks; k++) {
		const struct jit_block *b = &r->blocks[k];

		m->pcs[k] = b->pc;
		m->sizes[k] = (uint16_t)(b->next_pc - b->pc);
		m->counts[k] = (uint16_t)b->count;
		for (uint32_t i = 0; i < b->count; i++) {
			memcpy(m->bytes + at, &r->insns[b->first + i].orig, r->insns[b->first + i].len);
			at += r->insns[b->first + i].len;
		}
	}
	m->state = R_LIVE;
	m->strikes = 0;
	m->revived = 0;
	return 0;
}

/*
 * The blocks of region n start cold again in the table of counts, and need
 * to run more often than before to be translated again: at least `fails`
 * doublings, or one more than last time.
 */
static void region_cool(struct rv_jit *jit, uint32_t n, uint32_t fails)
{
	const struct jit_made *m = &jit->made[n];

	for (uint32_t k = 0; k < m->nblocks; k++) {
		struct jit_count *c = &jit->counts[hash_pc(m->pcs[k], jit->map_bits)];

		if (c->pc != m->pcs[k]) {
			c->pc = m->pcs[k];
			c->fails = 0;
			c->kind = 0;
		}
		c->count = 0;
		if (c->fails < FAILS_MAX)
			c->fails++;
		if (c->fails < fails)
			c->fails = (uint8_t)fails;
	}
}

static void region_drop(struct rv_jit *jit, uint32_t n);

/*
 * A region of batch b is gone. The batch's module is collected only once
 * none of its functions is in the table any more: when half of its regions
 * are gone, the others go too (and are translated again if they are run).
 */
static void batch_lose(struct rv_jit *jit, uint32_t b)
{
	if (b >= jit->nbatches || !jit->batch_live[b])
		return;
	jit->batch_live[b]--;
	if (!jit->batch_live[b] || jit->batch_live[b] * 2 > jit->batch_size[b])
		return;
	jit->batch_live[b] = 0;		/* (going: what follows does not come back here) */
	for (uint32_t n = 0; n < jit->nslots; n++)
		if (jit->made[n].batch == b + 1 && jit->made[n].state != R_FREE)
			region_drop(jit, n);
}

/* Region n is no more: its blocks leave the map, its number is free for another. */
static void region_drop(struct rv_jit *jit, uint32_t n)
{
	struct jit_made *m = &jit->made[n];
	uint32_t batch;

	for (uint32_t k = 0; k < m->nblocks; k++) {
		struct jit_entry *e = map_find(jit, m->pcs[k]);

		/* (If it is still this region's: another region may have the block too, and be the one the map names.) */
		if (e && e->slot == jit->slots[n]) {
			e->pc = MAP_GONE;
			e->slot = NO_SLOT;
			jit->map_dead++;
		}
	}
	slot_release(jit, jit->slots[n]);
	jit->code_kib -= m->kib;
	free(m->pcs);
	free(m->body);
	for (uint32_t i = 0; i < jit->npending; i++)
		if (jit->pending[i] == n)
			jit->pending[i] = jit->pending[--jit->npending];
	batch = m->batch;
	memset(m, 0, sizeof(*m));
	free(jit->targets[n]);
	jit->targets[n] = NULL;
	jit->used[n] = 0;
	jit->free_regions[jit->nfree++] = n;
	if (batch)
		batch_lose(jit, batch - 1);
}

/* Region n is not to be run until it has been looked over (revive()): no jump finds its blocks any more. */
static void region_suspend(struct rv_jit *jit, uint32_t n)
{
	struct jit_made *m = &jit->made[n];

	if (m->state != R_LIVE)
		return;
	for (uint32_t k = 0; k < m->nblocks; k++) {
		struct jit_entry *e = map_find(jit, m->pcs[k]);

		if (e && e->slot == jit->slots[n])
			e->pc |= 1;
	}
	m->state = R_SUSPENDED;
	jit->stat_suspended++;
}

static void pages_suspend(struct rv_jit *jit, uint64_t word, uint64_t first, uint64_t count)
{
	uint32_t n = (uint32_t)(word & ((1u << PAGE_REGION_BITS) - 1)) - 1;

	if ((word >> PAGE_REGION_BITS) - first < count && n < jit->nslots)
		region_suspend(jit, n);
}

/* The pages [first, first + count) are not what they were: suspend the regions made from them. */
static void suspend_pages(struct rv_jit *jit, uint64_t first, uint64_t count)
{
	if (jit->nfree == jit->nslots)
		return;
	if (!jit->pages) {
		/* (No table: every region's blocks, then.) */
		for (uint32_t n = 0; n < jit->nslots; n++) {
			const struct jit_made *m = &jit->made[n];

			for (uint32_t k = 0; m->state == R_LIVE && k < m->nblocks; k++)
				if ((m->pcs[k] >> RV_PAGE_SHIFT) - first < count ||
				    ((m->pcs[k] + m->sizes[k] - 1) >> RV_PAGE_SHIFT) - first < count)
					region_suspend(jit, n);
		}
		return;
	}
	if (count >= (1u << jit->pages_bits) / 8) {
		/* More pages than it is worth looking up one by one: all of the table. */
		for (uint32_t i = 0; i < 1u << jit->pages_bits; i++)
			if (jit->pages[i])
				pages_suspend(jit, jit->pages[i], first, count);
		return;
	}
	for (uint64_t page = first; page < first + count; page++) {
		uint32_t i = hash_page(page, jit->pages_bits);

		while (jit->pages[i]) {
			pages_suspend(jit, jit->pages[i], page, 1);
			i = (i + 1) & ((1u << jit->pages_bits) - 1);
		}
	}
}

/* Discard every translation, the suspended ones too. The table slots are kept, with their numbers. */
static void flush(struct rv_jit *jit)
{
	memset(jit->map, 0, ((size_t)1 << jit->map_bits) * sizeof(*jit->map));
	map_reserve(jit->map);
	memset(jit->counts, 0, ((size_t)1 << jit->map_bits) * sizeof(*jit->counts));
	jit->map_used = jit->map_dead = 0;
	for (uint32_t n = 0; n < jit->nslots; n++) {
		if (jit->made[n].state != R_FREE)
			slot_release(jit, jit->slots[n]);
		jit->used[n] = 0;
		free(jit->made[n].pcs);
		free(jit->made[n].body);
		memset(&jit->made[n], 0, sizeof(jit->made[n]));
		free(jit->targets[n]);
		jit->targets[n] = NULL;
		/* (The lowest number is handed out first.) */
		jit->free_regions[n] = jit->nslots - 1 - n;
	}
	jit->nfree = jit->nslots;
	jit->code_kib = 0;
	jit->npending = 0;
	jit->nbatches = 0;
	if (jit->pages)
		memset(jit->pages, 0, ((size_t)1 << jit->pages_bits) * sizeof(*jit->pages));
	jit->pages_used = 0;
	jit->stat_flushes++;
}

void rv_jit_flush(struct rv_cpu *cpu)
{
	struct rv_jit *jit = cpu->jit;

	if (jit && (jit->map_used || jit->nfree != jit->nslots))
		flush(jit);
}

/*
 * Room for translated code. The browser has one limit for all the WebAssembly
 * code of a page, every thread of every process together (V8's committed code
 * space, 4 GiB; when it is reached, the tab is gone: "Aw, Snap!"). A region is
 * a module, and a module takes at least a page of it (16 KiB on a Mac), so a
 * program the size of a browser, translated by many threads at once, reached
 * it in minutes. So:
 *
 *   - Whatever is discarded is let go of: its table slot is emptied
 *     (slot_release()), and the engine collects the module.
 *   - The host keeps account of what every thread's modules take, about
 *     (tools/patch-runtime.py, 15), and has a budget for all of them: when a
 *     thread is about to translate and all threads together are over it, the
 *     thread first lets go of its regions that have not been entered since it
 *     last looked (evict()). Translated code marks its region as entered (in
 *     cpu->jit_used, by region number) every time it is called, from the
 *     dispatcher or from another region. A thread also looks every so often
 *     while it runs (room_look()), so that one which is not translating any
 *     more gives up what it no longer uses as well.
 *   - Above the budget and a quarter, a thread translates only what it can make
 *     room for among its own: if it has nothing more to let go of, the block
 *     stays with the interpreter for the time being (it is counted again).
 *   - And a thread alone has at most code_cap_kib (RV_JIT_CODE_MB, 512 MiB
 *     by default, or half the budget if that is less). (Chromium's renderer
 *     thread, loading a page with 9 MB of script, came to the old 256 MiB and
 *     let go of regions it was still running.)
 * Regions let go of are translated again when they are run often enough.
 *
 * Letting go needs an engine that does not hold on to instances itself: up
 * to V8 13.3 (Chrome 133, Node 22) a table kept every instance that imported
 * it, for good (WasmTableObject::AddUse, "TODO(titzer): use weak cells here
 * to avoid leaking instances"), and every region imports the thread's table,
 * so there no module was ever freed before the program's exec or exit. From
 * V8 13.4 on the list is weak (WasmDispatchTable::AddUse). Measurements under
 * node-machine see the account, not the engine's code, go down.
 */
#define ROOM_PAGE_KIB	16

static uint64_t now_ns(void);
#define ROOM_LOOK	(64 * JIT_POLL)		/* instructions between the dispatcher's looks at the budget */
#define ROOM_EVERY_NS	200000000ULL		/* at most one round of evict() so often, unless over the cap */

/* What a module of len bytes takes of the engine's code space, about: as the host counts it. */
static uint32_t code_kib(uint64_t len)
{
	return (uint32_t)((len * 7 / 2 + 2048 + ROOM_PAGE_KIB * 1024 - 1) / (ROOM_PAGE_KIB * 1024) * ROOM_PAGE_KIB);
}

/* The host's budget and what all threads have, in KiB. 0 if it has no budget. */
static int room_query(struct rv_jit *jit, uint64_t *budget, uint64_t *sum)
{
#ifdef __wasm__
	long q;

	if (jit->no_room_query)
		return 0;
	q = __rv_jit_install(NULL, 0, -2);
	if (q < 0 || !((uint64_t)q >> 32)) {
		jit->no_room_query = q < 0;
		return 0;
	}
	*budget = (uint64_t)q >> 32;
	*sum = (uint64_t)q & 0xffffffffu;
	jit->room_budget = *budget;
	jit->room_sum = *sum;
	if (jit->code_cap_kib > *budget / 2)
		jit->code_cap_kib = *budget / 2;
	return 1;
#else
	(void)jit;
	(void)budget;
	(void)sum;
	return 0;
#endif
}

/*
 * Let go of the regions not entered since the last time (and of the suspended
 * ones); with `hard`, and still more than `down_to` KiB, of others too, in
 * the order of their numbers, until it is no more. Returns the KiB let go of.
 */
static uint64_t evict(struct rv_jit *jit, int hard, uint64_t down_to)
{
	uint64_t before = jit->code_kib, nregions = jit->stat_evicted;

	/* What translated code said, by address, into what this goes by: numbers. */
	for (uint32_t n = 0; jit->used_pc && n < jit->nslots; n++)
		if (jit->made[n].state != R_FREE && jit->used_pc[used_hash(jit->made[n].pcs[0])])
			jit->used[n] = 1;
	if (jit->used_pc)
		memset(jit->used_pc, 0, USED_PLACES);
	for (uint32_t n = 0; n < jit->nslots; n++) {
		if (jit->made[n].state == R_FREE)
			continue;
		if (jit->used[n] && jit->made[n].state == R_LIVE) {
			jit->used[n] = 0;
			continue;
		}
		region_drop(jit, n);
		jit->stat_evicted++;
	}
	for (uint32_t n = 0; hard && jit->code_kib > down_to && n < jit->nslots; n++) {
		if (jit->made[n].state == R_FREE)
			continue;
		region_drop(jit, n);
		jit->stat_evicted++;
	}
	if (jit->stat_evicted != nregions)
		jit->stat_evictions++;
	jit->stat_evicted_kib += before - jit->code_kib;
	if (jit->log_fd >= 0 && jit->stat_evicted != nregions) {
		char line[160];
		int n = snprintf(line, sizeof(line), "E %d %ld %llu %llu %llu %llu %llu %llu %d\n", (int)getpid(),
				 (long)syscall(SYS_gettid), (unsigned long long)(jit->stat_evicted - nregions),
				 (unsigned long long)(before - jit->code_kib), (unsigned long long)jit->code_kib,
				 (unsigned long long)jit->room_budget, (unsigned long long)jit->room_sum,
				 (unsigned long long)jit->stat_refused, hard);

		if (n > 0 && write(jit->log_fd, line, (size_t)n) < 0)
			jit->log_fd = -1;
	}
	return before - jit->code_kib;
}

/*
 * About to translate a region: is there room for it? Makes room among this
 * thread's own if there is too little. 1 if there is room (now).
 */
static int room_for(struct rv_jit *jit, uint64_t now)
{
	uint64_t budget = 0, sum = 0, need = 3 * ROOM_PAGE_KIB, ceiling;	/* (a region takes about three pages) */
	int has_budget = room_query(jit, &budget, &sum);
	int over_cap = jit->code_kib + need > jit->code_cap_kib;

	/* For testing: RV_JIT_REFUSE=n, every n-th time no room, as when other threads have used up the budget. */
	if (jit->refuse && ++jit->room_asked % jit->refuse == 0)
		return 0;
	if (!over_cap && (!has_budget || sum + need <= budget))
		return 1;
	if (over_cap || now - jit->last_evict_ns >= ROOM_EVERY_NS) {
		uint64_t freed;

		jit->last_evict_ns = now;
		/* Over the cap: down to three quarters of it, the cold ones first. */
		freed = evict(jit, over_cap, jit->code_cap_kib / 4 * 3);
		sum = sum > freed ? sum - freed : 0;
	}
	ceiling = budget + budget / 4;
	if (has_budget && sum + need > ceiling) {
		/* Far over: only if this thread can make the room among its own. */
		uint64_t excess = sum + need - ceiling;

		if (excess > jit->code_kib)
			return 0;
		evict(jit, 1, jit->code_kib - excess);
	}
	return jit->code_kib + need <= jit->code_cap_kib;
}

/* Every so often while the thread runs: if all threads together are over the budget, it gives up what it has not used. */
static void room_look(struct rv_cpu *cpu, struct rv_jit *jit)
{
	uint64_t budget, sum;

	jit->last_look = cpu->insn_count;
	if (jit->code_kib < 4 * ROOM_PAGE_KIB || !room_query(jit, &budget, &sum) || sum <= budget)
		return;
	if (now_ns() - jit->last_evict_ns < ROOM_EVERY_NS)
		return;
	jit->last_evict_ns = now_ns();
	evict(jit, 0, 0);
}

/*
 * The map has many places that were used and are not now, and suspended
 * regions nobody has come back to: those are discarded, and the map is
 * filled again, where it is, with the blocks of the regions in use. (Where it
 * is and as large as it is: translated code has both in it. So nothing that
 * is in use is lost.)
 */
static void map_tidy(struct rv_jit *jit)
{
	size_t places = (size_t)1 << jit->map_bits;
	struct jit_entry *old;
	uint16_t *old_len = NULL;

	if (!(old = malloc(places * sizeof(*old))) ||
	    (jit->map_len && !(old_len = malloc(places * sizeof(*old_len))))) {
		free(old);
		jit->stat_nomem++;
		return;
	}
	for (uint32_t n = 0; n < jit->nslots; n++)
		if (jit->made[n].state == R_SUSPENDED)
			region_drop(jit, n);
	memcpy(old, jit->map, places * sizeof(*old));
	if (old_len)
		memcpy(old_len, jit->map_len, places * sizeof(*old_len));
	memset(jit->map, 0, places * sizeof(*jit->map));
	map_reserve(jit->map);
	jit->map_used = jit->map_dead = 0;
	for (size_t i = 0; i < places; i++)
		if (old[i].pc && old[i].slot != NO_SLOT)
			map_insert(jit, old[i].pc, old[i].slot, old[i].index, old_len ? old_len[i] : 0);
	free(old);
	free(old_len);
	pages_remake(jit);
	jit->stat_tidied++;
}

/*
 * mm.c changed pages that code was translated from (count pages from page
 * `first`; count 0: everything): no thread may
 * run what it translated from them without looking first. Noted in the log
 * for every thread to see, this one included, before it next runs
 * translated code (catch_up()); the others are asked to leave what they are
 * running. The caller has the lock, unless it is the loader.
 */
void rv_jit_code_changed(struct rv_cpu *cpu, uint64_t first, uint64_t count, enum rv_code_cause cause)
{
	struct rv_proc *proc = cpu->proc;
	uint32_t epoch;

	/*
	 * The page table moved: translated code reads where it is from *cpu
	 * whenever a region is entered, and no thread is inside one now (the
	 * world is stopped, and a thread parks only outside translated code), so
	 * that is only counted.
	 */
	if (cause == RV_CODE_MOVED) {
		__atomic_add_fetch(&proc->code_events[cause], 1, __ATOMIC_SEQ_CST);
		return;
	}
	pthread_mutex_lock(&proc->lock);
	epoch = proc->code_epoch + 1;
	__atomic_store_n(&proc->code_log[epoch % RV_CODE_LOG].first, first, __ATOMIC_SEQ_CST);
	__atomic_store_n(&proc->code_log[epoch % RV_CODE_LOG].count, count, __ATOMIC_SEQ_CST);
	proc->code_events[cause]++;
	__atomic_store_n(&proc->code_epoch, epoch, __ATOMIC_SEQ_CST);
	for (struct rv_cpu *t = proc->threads; t; t = t->next_thread)
		if (t != cpu && t->jit)
			__atomic_or_fetch(&t->sig_pending, RV_SIG_CODE, __ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&proc->lock);
}

/*
 * Code changed since this thread last looked: suspend what it has from the
 * pages concerned, or discard everything if that is what it takes. The log
 * is read without the lock. The entry numbered n is written before the
 * count becomes n, and written over when the count is about to become
 * n + RV_CODE_LOG: so what was read of entry n is good if the count, looked
 * at afterwards, is still below n + RV_CODE_LOG - 1; a thread that is
 * further behind than that discards everything.
 */
static void catch_up(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;
	struct rv_jit *jit = cpu->jit;
	uint32_t now = __atomic_load_n(&proc->code_epoch, __ATOMIC_SEQ_CST);

	for (uint32_t n = jit->epoch + 1; n != now + 1; n++) {
		uint64_t first = __atomic_load_n(&proc->code_log[n % RV_CODE_LOG].first, __ATOMIC_SEQ_CST);
		uint64_t count = __atomic_load_n(&proc->code_log[n % RV_CODE_LOG].count, __ATOMIC_SEQ_CST);

		if (!count || __atomic_load_n(&proc->code_epoch, __ATOMIC_SEQ_CST) - n >= RV_CODE_LOG - 1) {
			rv_jit_flush(cpu);
			break;
		}
		suspend_pages(jit, first, count);
		jit->stat_events++;
	}
	jit->epoch = now;
}

/* RV_JIT_OFF bits. */
#define OFF_FP		1	/* floating point and CSRs inside regions */
#define OFF_CHAIN	2	/* jumps from region to region */
#define OFF_TARGETS	4	/* indirect jumps that stay in the region */
#define OFF_RETURNS	8	/* the block after a call joining the region */
#define OFF_WARM	16	/* only blocks that have run joining a region */
#define OFF_FPHOST	32	/* floating point done by the host's instructions in the region */
#define OFF_FMA		64	/* the host's fused multiply-add, where it has one */
#define OFF_STRUCT	128	/* the nesting of blocks as loops (struct layout): flat instead, every jump through the dispatcher */
#define OFF_PCACHE	256	/* the translation cache of memory accesses (check_access()): the page table every time */
#define OFF_JALRPROF	512	/* the targets the interpreter saw indirect jumps take (jalr_targets()): regions without them */
#define OFF_DIV		1024	/* division inline (gen_divrem()): by a call instead */
#define OFF_FPLOC	2048	/* floating-point registers and fcsr in locals (floc_*()): in *cpu at every instruction instead */

/* ----------------------------------------------------- indirect jump sites */

/* The interpreter executed the JALR at pc, which went to target (interp.c). */
void rv_jit_note_jalr(struct rv_cpu *cpu, uint64_t pc, uint64_t target)
{
	struct rv_jit *jit = cpu->jit;
	struct jalr_site *site = &jit->jalr[(uint32_t)(((pc >> 1) * HASH_MULTIPLIER) >> (64 - JALR_SITES_BITS))];

	if (site->pc != pc) {
		site->pc = pc;
		site->n = site->next = 0;
	}
	for (int i = 0; i < site->n; i++)
		if (site->to[i] == target)
			return;
	if (site->n < JALR_TARGETS) {
		site->to[site->n++] = target;
	} else {
		/* Full: the oldest goes. */
		site->to[site->next] = target;
		site->next = (uint8_t)((site->next + 1) % JALR_TARGETS);
	}
}

/* The targets seen for the JALR at pc, into `out` (room for JALR_TARGETS); how many. */
static int jalr_targets(const struct rv_jit *jit, uint64_t pc, uint64_t *out)
{
	const struct jalr_site *site = &jit->jalr[(uint32_t)(((pc >> 1) * HASH_MULTIPLIER) >> (64 - JALR_SITES_BITS))];

	if (site->pc != pc || (jit->off & OFF_JALRPROF))
		return 0;
	memcpy(out, site->to, site->n * sizeof(*out));
	return site->n;
}

/* ------------------------------------------------------------ region shape */

#define OPCODE(i)	((i) & 0x7f)
#define RD(i)		(((i) >> 7) & 31)
#define F3(i)		(((i) >> 12) & 7)
#define RS1(i)		(((i) >> 15) & 31)
#define RS2(i)		(((i) >> 20) & 31)
#define F7(i)		((i) >> 25)
#define IMM_I(i)	((int64_t)((int32_t)(i) >> 20))
#define IMM_S(i)	((int64_t)(int32_t)((((int32_t)(i) >> 25) << 5) | (((i) >> 7) & 31)))
#define IMM_U(i)	((int64_t)(int32_t)((i) & 0xfffff000))

static int64_t imm_b(uint32_t i)
{
	return (int32_t)(((int32_t)(i & 0x80000000) >> 19) | ((i & 0x80) << 4) |
			 ((i >> 20) & 0x7e0) | ((i >> 7) & 0x1e));
}

static int64_t imm_j(uint32_t i)
{
	return (int32_t)(((int32_t)(i & 0x80000000) >> 11) | (i & 0xff000) |
			 ((i >> 9) & 0x800) | ((i >> 20) & 0x7fe));
}

enum kind { K_NONE, K_PLAIN, K_BRANCH, K_JAL, K_JALR };

/*
 * The access path (2026-10-13; bits above rvjit-dispatch's, which go up to
 * 4096): the slow way of a load or store out of line in a block shared by the
 * region, not a call at every access (OFF_OUTLINE); one page check for a
 * block's accesses by the same base register (OFF_HOIST); branch hints on the
 * arms that are cold by construction (OFF_HINTS). See check_access_as().
 *
 * OFF_OUTLINE is set unless RV_JIT_OUTLINE=1: under V8 it made code slower
 * (node-machine, Node 22 with branch hints on: qsort 265 -> 326 ms, jbox2d
 * 5.13 -> 11.36 ms/step). Every continuation is a second way into the middle
 * of a block, and TurboFan lays the block out of line behind a jump with
 * register moves at each one. What the outline was for, keeping the call's
 * register saves off the fast path, the branch hint on the inline call's arm
 * does by itself (V8 puts the saves in the deferred arm): jbox2d 5.97 ->
 * 5.13 ms/step; leaving every failed access to the interpreter, no call at
 * all, measured no better (5.00).
 */
#define OFF_OUTLINE	8192
#define OFF_HOIST	16384
#define OFF_HINTS	32768

/*
 * An experiment, RV_JIT_TAG=1: what every memory access would cost more if
 * the page table were the translation cache of a whole machine. There the
 * table cannot have an entry for every page of the address space (2^27 of
 * them with 39-bit addresses, the kernel's at the top), so it is indexed by
 * the low bits of the page number and each entry names the page it is for;
 * an access loads that as well and compares (QEMU's and TinyEMU's
 * arrangement). Here: a second table, tag[i] == i for 2^21 entries, loaded
 * and compared before the usual test. It always matches: this measures the
 * instructions on the path taken, not the misses.
 */
#define TAG_BITS	21
static uint64_t *tag_table;
static pthread_once_t tag_once = PTHREAD_ONCE_INIT;

static void tag_init(void)
{
	uint64_t *t = malloc(sizeof(*t) << TAG_BITS);

	if (!t)
		return;
	for (uint64_t i = 0; i < (uint64_t)1 << TAG_BITS; i++)
		t[i] = i;
	tag_table = t;
}

/*
 * What the JIT translates, and how the instruction ends a block. The checks
 * mirror the interpreter's: an encoding it would reject is left to it.
 */
static enum kind classify(uint32_t i)
{
	uint32_t f3 = F3(i), f7 = F7(i);

	if (rv_zb_kind(i))
		return K_PLAIN;		/* Zba, Zbb, Zbs: gen_bitmanip() */
	switch (OPCODE(i)) {
	case 0x37: case 0x17:
		return K_PLAIN;
	case 0x6f:
		return K_JAL;
	case 0x67:
		return f3 == 0 ? K_JALR : K_NONE;
	case 0x63:
		return f3 == 2 || f3 == 3 ? K_NONE : K_BRANCH;
	case 0x03:
		return f3 <= 6 ? K_PLAIN : K_NONE;
	case 0x23:
		return f3 <= 3 ? K_PLAIN : K_NONE;
	case 0x13:
		if (f3 == 1)
			return (i >> 26) == 0 ? K_PLAIN : K_NONE;
		if (f3 == 5)
			return (i >> 26) == 0 || (i >> 26) == 0x10 ? K_PLAIN : K_NONE;
		return K_PLAIN;
	case 0x1b:
		if (f3 == 0)
			return K_PLAIN;
		if (f3 == 1)
			return f7 == 0 ? K_PLAIN : K_NONE;
		if (f3 == 5)
			return f7 == 0 || f7 == 0x20 ? K_PLAIN : K_NONE;
		return K_NONE;
	case 0x33:
		if (f7 == 0 || f7 == 1)
			return K_PLAIN;
		return f7 == 0x20 && (f3 == 0 || f3 == 5) ? K_PLAIN : K_NONE;
	case 0x3b:
		if (f7 == 0)
			return f3 == 0 || f3 == 1 || f3 == 5 ? K_PLAIN : K_NONE;
		if (f7 == 0x20)
			return f3 == 0 || f3 == 5 ? K_PLAIN : K_NONE;
		if (f7 == 1)
			return f3 == 0 || f3 >= 4 ? K_PLAIN : K_NONE;
		return K_NONE;
	case 0x0f:
		return f3 == 0 ? K_PLAIN : K_NONE;	/* FENCE; FENCE.I is the interpreter's */
	case 0x2f:
		/* The A extension, but for the minimum and maximum forms, which are rare. */
		if (f3 != 2 && f3 != 3)
			return K_NONE;
		switch (i >> 27) {
		case 0x02:
			return RS2(i) == 0 ? K_PLAIN : K_NONE;
		case 0x00: case 0x01: case 0x03: case 0x04: case 0x08: case 0x0c:
			return K_PLAIN;
		}
		return K_NONE;
	case 0x07: case 0x27:
		return f3 == 2 || f3 == 3 ? K_PLAIN : K_NONE;
	case 0x43: case 0x47: case 0x4b: case 0x4f: case 0x53:
		return K_PLAIN;		/* an invalid one is found out when it runs */
	case 0x73:
		/* Zicsr on fflags, frm and fcsr. (The counters would need an exact instruction count.) */
		return (f3 & 3) && (i >> 20) >= 1 && (i >> 20) <= 3 ? K_PLAIN : K_NONE;
	default:
		return K_NONE;
	}
}

/* Could code be translated from the page as it stands? Mapped, in memory (the interpreter runs its code first), executable. */
#define CODE_PAGE	(RV_PAGE_MAPPED | RV_PAGE_BACKED | RV_PROT_EXEC)

static int code_page_now(struct rv_cpu *cpu, uint64_t addr)
{
	uint64_t page = addr >> RV_PAGE_SHIFT;

	return page < cpu->npages && (cpu->map[page] & CODE_PAGE) == CODE_PAGE;
}

/*
 * A page code may be translated from: mapped, in memory, executable. The
 * page is marked as one that code was translated from, and with the mark it
 * stops allowing to be written as it stands, in one step with the look at its
 * state and before the code is read: whoever writes to the page or changes
 * it later finds the mark and says so (code_writable() and set_page() in
 * mm.c), and whoever changed it earlier has left a state that is seen here.
 */
static int code_page(struct rv_cpu *cpu, uint64_t addr)
{
	uint64_t page = addr >> RV_PAGE_SHIFT, e;

	if (page >= cpu->npages)
		return 0;
	e = __atomic_load_n(&cpu->map[page], __ATOMIC_SEQ_CST);
	for (;;) {
		if ((e & CODE_PAGE) != CODE_PAGE)
			return 0;
		if ((e & (RV_PAGE_CODE | RV_PROT_WRITE)) == RV_PAGE_CODE)
			return 1;
		if (__atomic_compare_exchange_n(&cpu->map[page], &e, (e | RV_PAGE_CODE) & ~(uint64_t)RV_PROT_WRITE, 0,
						__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return 1;
	}
}

/*
 * Fetch and classify the instruction at pc. K_NONE if it cannot be
 * translated. `mark`: it is to be translated (see code_page()), not just
 * looked at.
 */
static enum kind fetch_as(struct rv_cpu *cpu, uint64_t pc, struct jit_insn *insn, int mark)
{
	int (*page)(struct rv_cpu *, uint64_t) = mark ? code_page : code_page_now;
	uint16_t lo;
	uint32_t raw;

	if (!page(cpu, pc) || !page(cpu, pc + 1))
		return K_NONE;
	memcpy(&lo, rv_host_addr(cpu, pc), 2);
	insn->orig = lo;
	if ((lo & 3) != 3) {
		raw = rv_expand_compressed(lo);
		if (!raw)
			return K_NONE;
		insn->len = 2;
	} else {
		uint16_t hi;

		/* (The second half may be in the next page, which may be elsewhere.) */
		if (!page(cpu, pc + 3))
			return K_NONE;
		memcpy(&hi, rv_host_addr(cpu, pc + 2), 2);
		raw = (uint32_t)lo | (uint32_t)hi << 16;
		insn->orig = raw;
		insn->len = 4;
	}
	insn->pc = pc;
	insn->raw = raw;
	if (cpu->jit->off & OFF_FP) {
		switch (OPCODE(raw)) {
		case 0x07: case 0x27: case 0x43: case 0x47: case 0x4b: case 0x4f: case 0x53: case 0x73:
			return K_NONE;
		}
	}
	return classify(raw);
}

static enum kind fetch(struct rv_cpu *cpu, uint64_t pc, struct jit_insn *insn)
{
	return fetch_as(cpu, pc, insn, 1);
}

static int find_block(const struct jit_region *r, uint64_t pc)
{
	for (int i = 0; i < r->nblocks; i++)
		if (r->blocks[i].pc == pc)
			return i;
	return -1;
}

/*
 * Has the block at pc been running lately? Following every branch a region
 * could take fills it with code that never runs (error paths, mostly) and
 * leaves no room for what the hot code calls. So a block gets in only if the
 * interpreter has run it while the region's first block was getting hot, or
 * if it has been translated before, as part of another region. A block left
 * out is an exit, like any other code outside the region; if it turns hot
 * later it starts a region of its own.
 *
 * Once is enough (JIT_WARM). Asking for more leaves out blocks that do run:
 * with 25, sort went back to the dispatcher 5 million times instead of 50
 * thousand. (The counts are in a small direct-mapped table, so some are lost.)
 */
static int warm(struct rv_jit *jit, uint64_t pc)
{
	const struct jit_count *c = &jit->counts[hash_pc(pc, jit->map_bits)];

	if (c->pc == pc && c->count >= jit->warm)
		return 1;
	return map_find(jit, pc) != NULL;
}

/* Was the block at pc in the region as it was made before (jit->hint, sorted)? */
static int hinted(const struct rv_jit *jit, uint64_t pc)
{
	uint32_t lo = 0, hi = jit->nhint;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (jit->hint[mid] == pc)
			return 1;
		if (jit->hint[mid] < pc)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* Queue the block starting at pc, unless it is known, cold, untranslatable or there is no room. */
static void want_block(struct rv_cpu *cpu, struct jit_region *r, uint64_t pc)
{
	struct jit_insn probe;

	if (r->nblocks == JIT_MAX_BLOCKS || find_block(r, pc) >= 0)
		return;
	/*
	 * The first block is the hot one that started this; the others have to
	 * have been running too. (Or, for a region made again as it was made
	 * before, to have been in it then: see hint_translate().)
	 */
	if (r->nblocks && !(cpu->jit->off & OFF_WARM) && !(cpu->jit->nhint ? hinted(cpu->jit, pc) : warm(cpu->jit, pc)))
		return;
	if (fetch(cpu, pc, &probe) == K_NONE)
		return;
	r->blocks[r->nblocks].pc = pc;
	r->blocks[r->nblocks].count = 0;
	r->blocks[r->nblocks].is_return = 0;
	r->nblocks++;
}

/* Collect the region reachable from `entry`. Blocks are decoded in the order they were found. */
static void build_region(struct rv_cpu *cpu, struct jit_region *r, uint64_t entry)
{
	r->nblocks = r->ninsns = 0;
	want_block(cpu, r, entry);

	for (int b = 0; b < r->nblocks; b++) {
		struct jit_block *blk = &r->blocks[b];
		uint64_t pc = blk->pc;
		enum kind kind = K_PLAIN;

		blk->first = (uint32_t)r->ninsns;
		while (blk->count < JIT_BLOCK_INSNS && r->ninsns < JIT_MAX_INSNS) {
			struct jit_insn *insn = &r->insns[r->ninsns];

			kind = fetch(cpu, pc, insn);
			if (kind == K_NONE)
				break;
			r->ninsns++;
			blk->count++;
			pc += insn->len;
			if (kind != K_PLAIN)
				break;
		}
		blk->next_pc = pc;

		if (!blk->count) {
			/* Out of room before its first instruction: drop it and what follows. */
			r->nblocks = b;
			break;
		}

		/* Follow the static successors. */
		if (kind == K_BRANCH) {
			const struct jit_insn *last = &r->insns[r->ninsns - 1];

			want_block(cpu, r, last->pc + (uint64_t)imm_b(last->raw));
			want_block(cpu, r, pc);
		} else if (kind == K_JAL || kind == K_JALR) {
			const struct jit_insn *last = &r->insns[r->ninsns - 1];

			if (kind == K_JAL) {
				want_block(cpu, r, last->pc + (uint64_t)imm_j(last->raw));
			} else {
				/*
				 * Where the interpreter saw this jump go: a return to a
				 * caller in the region, the cases of a switch, the
				 * handlers of an interpreter's dispatch. Without them a
				 * region would end at every indirect jump.
				 */
				uint64_t targets[JALR_TARGETS];
				int n = jalr_targets(cpu->jit, last->pc, targets);

				for (int i = 0; i < n; i++)
					want_block(cpu, r, targets[i]);
			}
			/*
			 * A call: the callee returns to the next instruction. With that
			 * block in the region too, the return need not leave it.
			 */
			if (RD(last->raw) && !(cpu->jit->off & OFF_RETURNS)) {
				int k;

				want_block(cpu, r, pc);
				if ((k = find_block(r, pc)) >= 0)
					r->blocks[k].is_return = 1;
			}
		} else {
			want_block(cpu, r, pc);	/* the block was cut short: fall through */
		}
	}
}

/* -------------------------------------------------------------- code output */

/*
 * The shape of a region's code (gen_body()). The blocks are nested as the
 * loops of the guest's code are, so that most jumps are direct branches:
 *
 *   block EXIT
 *     loop TOP                      ; the region's dispatcher
 *       block ITEM_n-1 ... block ITEM_0
 *         leave if a signal is pending or the budget is spent
 *         br_table $next -> the item that has block $next
 *       end  ; item 0's code
 *       ...
 *       end  ; item n-1's code
 *     end
 *   end
 *
 * An item is a block of the region, or a loop of the guest's: a strongly
 * connected part of its control flow, with the block it is entered at as its
 * header, nested the same way (loop L; block ladder; the loop's items; end).
 * The items of a scope are in topological order of the edges that remain
 * when those into the header are taken away, so a jump to a later item of
 * the same scope, or of an enclosing one, is a plain `br` to its block, and
 * a jump back to the header of a loop is `br` to the loop. The loop's top
 * tests whether $next is its header: if not, the loop was entered from
 * outside at some other of its blocks (the dispatcher above, or an indirect
 * jump), and its own br_table dispatches on $next, which may go on into a
 * nested loop the same way. So every block can still be entered from
 * anywhere by setting $next and branching to the loop that has it, and the
 * signal and budget check is where every cycle passes: at the top. What is
 * neither forward nor a header (an irreducible jump) goes through TOP.
 *
 * Indirect jumps go to the lookup (gen_lookup()) of the innermost scope that
 * has them: an item of its own, right after the header, selected through the
 * scope's dispatcher with $next = nblocks + scope; from there every target
 * is a branch as above. The layout is made by layout_region() from the
 * blocks' static successors, the return addresses of calls, and the targets
 * the interpreter saw indirect jumps take (jalr_note()).
 */
#define SCOPE_MAX	(JIT_MAX_BLOCKS + 1)
#define ITEM_LOOKUP	(-1)
#define ITEM_SCOPE(s)	(-2 - (s))
#define SCOPE_OF_ITEM(v) (-2 - (v))

struct scope {
	int parent;		/* -1 for the top */
	int parent_item;	/* index of its item in the parent */
	int header;		/* block index; -1 for the top */
	int first, nitems;	/* its items: layout.items[first .. first + nitems) */
	int lookup;		/* index of its lookup item, or -1 */
	int lo, hi;		/* the blocks inside it, nested scopes' included, are numbered lo..hi (layout_renumber()) */
};

struct layout {
	int nscopes;
	struct scope scopes[SCOPE_MAX];
	int items[2 * JIT_MAX_BLOCKS + 2 * SCOPE_MAX];	/* a block index, ITEM_LOOKUP, or ITEM_SCOPE(s) */
	int nitems;
	int block_scope[JIT_MAX_BLOCKS];	/* the scope each block is a direct item of */
	int block_item[JIT_MAX_BLOCKS];	/* ... and its index there */
	uint64_t succ[JIT_MAX_BLOCKS][(JIT_MAX_BLOCKS + 63) / 64];	/* edges, as bit sets */
};

struct gen {
	struct rv_cpu *cpu;
	struct wbuf *b;
	const struct jit_region *r;
	const struct layout *lay;
	int scope, item;	/* where code is being generated: the item (-1: the scope's dispatcher) */
	uint32_t used, written;	/* guest registers, as bit masks */
	int hoist, hints;
	/* OFF_HOIST: for the block being generated, by instruction: its part in a group of accesses (ACC_*). */
	uint8_t role[JIT_BLOCK_INSNS], slot[JIT_BLOCK_INSNS], uprot[JIT_BLOCK_INSNS];
	uint32_t j;		/* the instruction of the block being generated */
	int bad;		/* the code would be wrong (a count did not match): not to be used */
	int8_t cache_slot[32];	/* the translation cache slot of each register, or -1 (cache_slots()) */
	uint32_t bases;		/* the slots in use, as a bit mask (set up on entry) */
	/* OFF_FPLOC: floating-point registers in locals (floc_*()). fcsr is the local at fbase, f registers follow it as they are first used. */
	int floc;		/* on for this region */
	uint32_t fbase;		/* the first local of the group */
	int8_t fmap[32];	/* the local (after fcsr's) of each f register, or -1 */
	int nf;			/* f registers with a local */
	uint32_t fwritten;	/* ... those written, as a bit mask */
	int fc_used, fc_written;
};
enum { ACC_PLAIN, ACC_FIRST, ACC_MEMBER };

/*
 * Label depths from where code is being generated. The labels open, from the
 * inside out: the item blocks after the current one in its scope, the
 * scope's loop, then the same for each enclosing scope, then EXIT. `extra`
 * counts blocks and ifs the caller has opened around the branch.
 */
static uint32_t depth_to_loop(const struct gen *g, int scope, uint32_t extra)
{
	int s = g->scope, cur = g->item;
	uint32_t depth = extra;

	for (;;) {
		depth += (uint32_t)(g->lay->scopes[s].nitems - 1 - cur);
		if (s == scope)
			return depth;
		depth++;
		cur = g->lay->scopes[s].parent_item;
		s = g->lay->scopes[s].parent;
	}
}

static uint32_t depth_to_item(const struct gen *g, int scope, int item, uint32_t extra)
{
	int s = g->scope, cur = g->item;
	uint32_t depth = extra;

	while (s != scope) {
		depth += (uint32_t)(g->lay->scopes[s].nitems - 1 - cur) + 1;
		cur = g->lay->scopes[s].parent_item;
		s = g->lay->scopes[s].parent;
	}
	return depth + (uint32_t)(item - cur - 1);
}

/* Room for n branch hints in jit->hints: 0, or -1 if there is none. */
static int hints_room(struct rv_jit *jit, uint32_t n)
{
	uint32_t cap = jit->hints_cap ? jit->hints_cap : 1024, *h;

	if (n <= jit->hints_cap)
		return 0;
	while (cap < n)
		cap *= 2;
	if (!(h = realloc(jit->hints, cap * sizeof(*h))))
		return -1;
	jit->hints = h;
	jit->hints_cap = cap;
	return 0;
}

/* Where hints go (RV_JIT_HINT_SITES=mask, default all: for measuring them one by one; in the cache key, cache_switches()). */
enum { HS_ACCESS = 1, HS_RESERVE = 2, HS_FAULT = 4, HS_FP = 8, HS_POLL = 16, HS_ALL = 31 };

/*
 * A branch hint (OFF_HINTS) for the `if` or `br_if` about to be written:
 * likely or not. Kept as the offset in the body, which gen_module() makes the
 * offset in the function.
 */
static void hint(struct gen *g, uint32_t site, int likely)
{
	struct rv_jit *jit = g->cpu->jit;

	if (!g->hints || !(jit->hint_sites & site))
		return;
	if (hints_room(jit, jit->nhints + 1)) {
		g->hints = 0;	/* no hints for this one, then */
		jit->nhints = 0;
		return;
	}
	jit->hints[jit->nhints++] = (uint32_t)g->b->len << 1 | (likely ? 1 : 0);
}

static uint32_t exit_depth(const struct gen *g, uint32_t extra)
{
	return depth_to_loop(g, 0, extra) + 1;
}

/* The item of scope `a` that `s` (a descendant of a, or a itself with item `cur`) is inside; -1 if s is not inside a. */
static int item_at(const struct layout *lay, int a, int s, int cur)
{
	while (s != a) {
		if (s < 0)
			return -1;
		cur = lay->scopes[s].parent_item;
		s = lay->scopes[s].parent;
	}
	return cur;
}

static int scope_depth(const struct layout *lay, int s)
{
	int d = 0;

	for (; s > 0; s = lay->scopes[s].parent)
		d++;
	return d;
}

static void set_next(struct gen *g, int value)
{
	w_i32(g->b, value);
	w_local(g->b, W_LOCAL_SET, L_NEXT);
}

/* Go on at block t of the region, from where code is being generated. */
static void branch_to_block(struct gen *g, int t, uint32_t extra)
{
	const struct layout *lay = g->lay;
	int ts = lay->block_scope[t], a = g->scope, b = ts;
	int da = scope_depth(lay, a), db = scope_depth(lay, b);

	/* The lowest scope that has both. */
	while (da > db) {
		a = lay->scopes[a].parent;
		da--;
	}
	while (db > da) {
		b = lay->scopes[b].parent;
		db--;
	}
	while (a != b) {
		a = lay->scopes[a].parent;
		b = lay->scopes[b].parent;
	}
	{
		int here = item_at(lay, a, g->scope, g->item);
		int there = item_at(lay, a, ts, lay->block_item[t]);

		if (ts == a && a > 0 && lay->scopes[a].header == t) {
			/* Back to the header of a loop we are in. */
			set_next(g, t);
			w_br(g->b, W_BR, depth_to_loop(g, a, extra));
		} else if (there > here && ts == a) {
			w_br(g->b, W_BR, depth_to_item(g, a, there, extra));
		} else if (there > here) {
			/* Into a loop we are not in: its dispatcher goes on. */
			set_next(g, t);
			w_br(g->b, W_BR, depth_to_item(g, a, there, extra));
		} else {
			/* Backwards, and not to a header: the region's dispatcher (which may leave: pc has to be right). */
			w_i64(g->b, g->r->blocks[t].pc);
			w_local(g->b, W_LOCAL_SET, L_PC);
			set_next(g, t);
			w_br(g->b, W_BR, depth_to_loop(g, 0, extra));
		}
	}
}

static void get_reg(struct gen *g, uint32_t r)
{
	if (!r) {
		w_i64(g->b, 0);
		return;
	}
	g->used |= 1u << r;
	w_local(g->b, W_LOCAL_GET, L_REG(r));
}

/* Pop the value on the stack into register r (x0 discards it). */
static void set_reg(struct gen *g, uint32_t r)
{
	if (!r) {
		w_op(g->b, 0x1a);	/* drop */
		return;
	}
	g->used |= 1u << r;
	g->written |= 1u << r;
	w_local(g->b, W_LOCAL_SET, L_REG(r));
}

/* Leave the region for the jump target in L_PC; the function's end sees whether a region has it. */
static void leave_for_target(struct gen *g, uint32_t extra)
{
	w_i32(g->b, !(g->cpu->jit->off & OFF_CHAIN));
	w_local(g->b, W_LOCAL_SET, L_CHAIN);
	w_br(g->b, W_BR, exit_depth(g, extra));
}

/* Continue at `target`: inside the region if it has that block, else leave. */
static void transition(struct gen *g, uint64_t target, uint32_t extra)
{
	int k = find_block(g->r, target);

	if (k >= 0) {
		branch_to_block(g, k, extra);
	} else {
		w_i64(g->b, target);
		w_local(g->b, W_LOCAL_SET, L_PC);
		leave_for_target(g, extra);
	}
}

/*
 * Leave with pc at an instruction that has not been executed. The block's
 * whole length was charged to the budget on entry; give back the part that
 * did not run.
 */
static void bail(struct gen *g, const struct jit_insn *insn, uint32_t not_run, uint32_t extra)
{
	w_local(g->b, W_LOCAL_GET, L_BUDGET);
	w_i32(g->b, (int32_t)not_run);
	w_op(g->b, W_I32_ADD);
	w_local(g->b, W_LOCAL_SET, L_BUDGET);
	w_i64(g->b, insn->pc);
	w_local(g->b, W_LOCAL_SET, L_PC);
	w_br(g->b, W_BR, exit_depth(g, extra));
}

/*
 * The guest address is in L_ADDR. Leave unless [addr, addr + size) lies in
 * one page that allows `prot` as it stands; otherwise the host address is in
 * L_HOST afterwards. This is rv_ptr() (rv.h): the page's entry in the page
 * table has what the page allows in its low bits and, above them, what to
 * add to the guest address. The page index is clamped to npages, where the
 * table has an entry that allows nothing, so one test covers "outside the
 * address space" as well, and a page that has no memory yet allows nothing
 * either: the interpreter, which the region is left for, gives it some.
 */
static void check_access_as(struct gen *g, const struct jit_insn *insn, uint32_t not_run,
			    unsigned size, unsigned prot, int aligned, uint32_t base);
static void call_helper(struct gen *g, int helper);

/*
 * Which registers get a translation cache: the PCACHE_SLOTS (or RV_JIT_PCACHE)
 * that address memory most in the region, by a count of the instructions.
 * The cache lives in locals; a local for every register was tried, and
 * TurboFan, with that many values live through the whole function, spilled
 * and shuffled them around the miss path's call on the fast path.
 */
static void cache_slots(struct gen *g)
{
	uint32_t count[32] = { 0 }, defs[32] = { 0 };
	int slots = g->cpu->jit->pcache_slots;

	memset(g->cache_slot, -1, sizeof(g->cache_slot));
	if (!slots || (g->cpu->jit->off & OFF_PCACHE) || g->cpu->jit->flatmem_on || g->cpu->jit->tag)
		return;
	for (int i = 0; i < g->r->ninsns; i++) {
		uint32_t raw = g->r->insns[i].raw;

		switch (OPCODE(raw)) {
		case 0x03:
			count[RS1(raw)]++;
			defs[RD(raw)]++;	/* (loaded: a pointer followed) */
			break;
		case 0x07: case 0x23: case 0x27:
			count[RS1(raw)]++;
			break;
		}
	}
	/*
	 * A register that is loaded from memory about as often as it addresses
	 * memory is a pointer followed from object to object (a list walked
	 * down, a temporary holding what was just loaded): on another page as
	 * often as not, and a miss costs more than the check does. No cache for
	 * it. A register that advances by arithmetic (an array's address, the
	 * stack pointer, a frame or base pointer) is the one whose last page is
	 * the next page as well.
	 */
	count[0] = 0;
	for (int r = 1; r < 32; r++)
		if (defs[r] * 2 > count[r])
			count[r] = 0;
	for (int k = 0; k < slots; k++) {
		int best = -1;

		for (int r = 1; r < 32; r++)
			if (count[r] && (best < 0 || count[r] > count[best]))
				best = r;
		if (best < 0)
			break;
		g->cache_slot[best] = (int8_t)k;
		count[best] = 0;
	}
}

/*
 * The access at L_ADDR is through register `base` (0: none): if the register
 * has a translation cache (cache_slots()), that is tried first. It has the
 * page of the register's last access: an access that lies within that page,
 * and that the page allows (reading and writing are cached apart), needs
 * nothing but a subtraction, a comparison and an addition, where the page
 * table needs the clamp, the load, the test of the flags and the test for a
 * page crossing. Only a miss does all that, in the module's own function
 * (gen_xlat_fn()), which leaves the cache's new contents in cpu->xlat. The
 * cache is in locals, so it is empty when the region is entered and gone
 * when it is left, and what it holds is at most JIT_BUDGET instructions old:
 * changes to a page's protection by another thread (mprotect(), code being
 * marked) are seen by the next region call at the latest; this thread's own
 * changes are all system calls, outside translated code.
 */
static void check_access(struct gen *g, const struct jit_insn *insn, uint32_t not_run,
			 unsigned size, unsigned prot, uint32_t base)
{
	struct wbuf *b = g->b;
	int k = base ? g->cache_slot[base] : -1;

	if (k < 0) {
		check_access_as(g, insn, not_run, size, prot, 0, 0);
		return;
	}
	g->bases |= 1u << k;
	/* addr - page < 4096 - (size - 1): in the cached page, the last byte too */
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_local(b, W_LOCAL_GET, prot & RV_PROT_WRITE ? L_CB_W(k) : L_CB_R(k));
	w_op(b, W_I64_SUB);
	w_i64(b, RV_PAGE_SIZE - size + 1);
	w_op(b, W_I64_LT_U);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_local(b, W_LOCAL_GET, L_CD(k));
	w_op(b, W_I64_ADD);
	w_local(b, W_LOCAL_SET, L_HOST);
	w_byte(b, W_ELSE);
	/* A miss: the module's function does the whole check and leaves the cache's new contents in cpu->xlat. */
	w_local(b, W_LOCAL_GET, L_CPU);
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_i64(b, size | prot << 8);
	call_helper(g, F_XLAT);
	w_local(b, W_LOCAL_TEE, L_HOST);
	w_op(b, W_I64_EQZ);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	bail(g, insn, not_run, 2);
	w_op(b, W_END);
	if (g->cpu->verbose) {
		/* (-v: count the misses, for region-bench.js and the report.) */
		w_local(b, W_LOCAL_GET, L_CPU);
		w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, jit));
		w_local(b, W_LOCAL_TEE, L_TMP);
		w_local(b, W_LOCAL_GET, L_TMP);
		w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_jit, stat_misses));
		w_i64(b, 1);
		w_op(b, W_I64_ADD);
		w_mem(b, W_I64_STORE, 3, offsetof(struct rv_jit, stat_misses));
	}
	for (int j = 0; j < 3; j++) {
		w_local(b, W_LOCAL_GET, L_CPU);
		w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, xlat) + 8 * j);
		w_local(b, W_LOCAL_SET, j == 0 ? L_CB_R(k) : j == 1 ? L_CB_W(k) : L_CD(k));
	}
	w_op(b, W_END);
}

/*
 * The same; with `aligned`, also leave unless the address is a multiple of the size (which keeps it in one page).
 *
 * The slow way (the test failed: no page in the table that allows this as it
 * stands, across two pages, misaligned for an atomic, or a reservation's
 * page) is the module's own function for it (gen_reserve_fn()), which gives
 * the host address or 0, and 0 leaves for the interpreter. That call is right
 * there, at every access (the shared slow block that OFF_OUTLINE was is gone: it
 * made code slower under V8, see the git history).
 *
 * OFF_HOIST: in a block, the accesses by one base register that is not
 * written in between form a group (gen_groups()). The first checks its
 * page as above for what all of them do (read and/or write) and keeps the
 * page's guest address and host - guest in a slot's two locals; the others
 * only test that they are in that page (one subtraction and compare), with
 * the slow way as above if not. The slot is good for the rest of the block
 * only: like a pending change of code (RV_SIG_CODE), a change of the page
 * table by another thread is seen at the end of the block at the latest.
 * Nothing the block itself calls changes the table (rv_jit_reserve() only
 * looks).
 */
static void check_access_as(struct gen *g, const struct jit_insn *insn, uint32_t not_run,
			    unsigned size, unsigned prot, int aligned, uint32_t base)
{
	struct wbuf *b = g->b;
	int tag = g->cpu->jit->tag && tag_table;
	int role = g->hoist && !aligned && !tag ? g->role[g->j] : ACC_PLAIN;
	uint32_t s = g->slot[g->j];
	uint64_t how;

	if (role == ACC_FIRST)
		prot = g->uprot[g->j];
	how = size | prot << 8 | (aligned ? 1 << 16 : 0);

	(void)base;

	if (g->cpu->jit->flatmem_on) {
		(void)insn; (void)not_run; (void)size; (void)prot; (void)aligned;
		w_local(b, W_LOCAL_GET, L_ADDR);
		w_i64(b, g->cpu->jit->flatmem);
		w_op(b, W_I64_ADD);
		w_local(b, W_LOCAL_SET, L_HOST);
		return;
	}

	if (role == ACC_MEMBER) {
		/* addr - page >u 4096 - size: not in the page the group was checked for (or across its end) */
		w_local(b, W_LOCAL_GET, L_ADDR);
		w_local(b, W_LOCAL_GET, L_CPAGE(s));
		w_op(b, W_I64_SUB);
		w_i64(b, RV_PAGE_SIZE - size);
		w_op(b, W_I64_GT_U);
	} else {
		if (tag) {
			/* tag[(addr >> 12) & mask] != addr >> 12, left on the stack for the test below */
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, RV_PAGE_SHIFT);
			w_op(b, W_I64_SHR_U);
			w_i64(b, ((uint64_t)1 << TAG_BITS) - 1);
			w_op(b, W_I64_AND);
			w_i64(b, 3);
			w_op(b, W_I64_SHL);
			w_mem(b, W_I64_LOAD, 3, (uint64_t)(uintptr_t)tag_table);
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, RV_PAGE_SHIFT);
			w_op(b, W_I64_SHR_U);
			w_op(b, W_I64_NE);
		}

		w_local(b, W_LOCAL_GET, L_ADDR);
		w_i64(b, RV_PAGE_SHIFT);
		w_op(b, W_I64_SHR_U);
		w_local(b, W_LOCAL_TEE, L_HOST);
		w_local(b, W_LOCAL_GET, L_NPAGES);
		w_local(b, W_LOCAL_GET, L_HOST);
		w_local(b, W_LOCAL_GET, L_NPAGES);
		w_op(b, W_I64_LT_U);
		w_op(b, W_SELECT);
		w_i64(b, 3);
		w_op(b, W_I64_SHL);
		w_local(b, W_LOCAL_GET, L_MAP);
		w_op(b, W_I64_ADD);
		w_mem(b, W_I64_LOAD, 3, 0);
		w_local(b, W_LOCAL_TEE, L_HOST);
		w_op(b, W_I32_WRAP_I64);
		w_i32(b, (int32_t)prot);
		w_op(b, W_I32_AND);
		w_i32(b, (int32_t)prot);
		w_op(b, W_I32_NE);
		if (aligned) {
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, size - 1);
			w_op(b, W_I64_AND);
			w_i64(b, 0);
			w_op(b, W_I64_NE);
			w_op(b, W_I32_OR);
		} else if (size > 1) {
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, RV_PAGE_SIZE - 1);
			w_op(b, W_I64_AND);
			w_i64(b, RV_PAGE_SIZE - size);
			w_op(b, W_I64_GT_U);
			w_op(b, W_I32_OR);
		}
		if (tag)
			w_op(b, W_I32_OR);
	}
	hint(g, HS_ACCESS, 0);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	/*
	 * Beyond the table: a reservation, perhaps (mm.c), where programs that
	 * reserve keep their heaps. The page is found by the module's own
	 * function for that (gen_reserve_fn()), which gives the host address
	 * or 0, and 0 leaves. L_HOST is made what it would be for a page in the
	 * table (the host address minus the guest address has no bits in
	 * RV_PAGE_FLAGS: the two are as far into their pages), so the code below
	 * is the same for both. The function answers for an address in the
	 * table as well (that access failed for another reason, or is a group's
	 * in another page), so there is one call and one way out here, not a
	 * test and two (a seventh less code: codegen-regs, 442ff04).
	 */
	w_local(b, W_LOCAL_GET, L_CPU);
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_i64(b, how);
	call_helper(g, F_RESERVE);
	w_local(b, W_LOCAL_TEE, L_HOST);
	w_op(b, W_I64_EQZ);
	hint(g, HS_RESERVE, 0);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	bail(g, insn, not_run, 2);
	w_op(b, W_END);
	w_local(b, W_LOCAL_GET, L_HOST);
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_op(b, W_I64_SUB);
	w_local(b, W_LOCAL_SET, L_HOST);
	if (role == ACC_MEMBER) {
		w_byte(b, W_ELSE);
		w_local(b, W_LOCAL_GET, L_CDELTA(s));
		w_local(b, W_LOCAL_SET, L_HOST);
	}
	w_op(b, W_END);
	if (role == ACC_MEMBER) {
		w_local(b, W_LOCAL_GET, L_HOST);
		w_local(b, W_LOCAL_GET, L_ADDR);
		w_op(b, W_I64_ADD);
		w_local(b, W_LOCAL_SET, L_HOST);
		return;
	}
	w_local(b, W_LOCAL_GET, L_HOST);
	w_i64(b, ~(uint64_t)RV_PAGE_FLAGS);
	w_op(b, W_I64_AND);
	if (role == ACC_FIRST)
		w_local(b, W_LOCAL_TEE, L_CDELTA(s));
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_op(b, W_I64_ADD);
	w_local(b, W_LOCAL_SET, L_HOST);
	if (role == ACC_FIRST) {
		w_local(b, W_LOCAL_GET, L_ADDR);
		w_i64(b, ~(RV_PAGE_SIZE - 1));
		w_op(b, W_I64_AND);
		w_local(b, W_LOCAL_SET, L_CPAGE(s));
	}
}

/* L_ADDR = rs1 + imm */
static void effective_address(struct gen *g, uint32_t rs1, int64_t imm)
{
	get_reg(g, rs1);
	if (imm) {
		w_i64(g->b, (uint64_t)imm);
		w_op(g->b, W_I64_ADD);
	}
	w_local(g->b, W_LOCAL_SET, L_ADDR);
}

/* Leaves the 32-bit result of the value on the stack sign-extended to 64 bits. */
static void sext32(struct gen *g)
{
	w_op(g->b, W_I32_WRAP_I64);
	w_op(g->b, W_I64_EXTEND_I32_S);
}

static void call_helper(struct gen *g, int helper)
{
	w_byte(g->b, W_CALL);
	w_uleb(g->b, (uint64_t)helper);
}

/* Register-register and register-immediate arithmetic share this: the operands are on the stack. */
static void alu(struct gen *g, uint32_t f3, int sub_or_sra, int word)
{
	struct wbuf *b = g->b;

	if (word) {
		switch (f3) {
		case 0:
			w_op(b, sub_or_sra ? W_I64_SUB : W_I64_ADD);
			sext32(g);
			break;
		default: { /* shifts act on the low 32 bits: both operands to i32 */
			/* stack: a b -> wrap both. b is on top; park it. */
			w_op(b, W_I32_WRAP_I64);
			w_local(b, W_LOCAL_SET, L_TMP32);
			w_op(b, W_I32_WRAP_I64);
			w_local(b, W_LOCAL_GET, L_TMP32);
			w_op(b, f3 == 1 ? W_I32_SHL : sub_or_sra ? W_I32_SHR_S : W_I32_SHR_U);
			w_op(b, W_I64_EXTEND_I32_S);
			break;
		}
		}
		return;
	}

	switch (f3) {
	case 0: w_op(b, sub_or_sra ? W_I64_SUB : W_I64_ADD); break;
	case 1: w_op(b, W_I64_SHL); break;
	case 2: w_op(b, W_I64_LT_S); w_op(b, W_I64_EXTEND_I32_U); break;
	case 3: w_op(b, W_I64_LT_U); w_op(b, W_I64_EXTEND_I32_U); break;
	case 4: w_op(b, W_I64_XOR); break;
	case 5: w_op(b, sub_or_sra ? W_I64_SHR_S : W_I64_SHR_U); break;
	case 6: w_op(b, W_I64_OR); break;
	case 7: w_op(b, W_I64_AND); break;
	}
}

/*
 * Floating-point registers and fcsr in locals (OFF_FPLOC), as the integer
 * registers are: the ones a region uses are read from *cpu when it is
 * entered and the ones it writes stored when it is left; only a call to
 * rv_jit_fp() (gen_fp_call()) sees *cpu, so the operands go there before it
 * and the results come back after it. A local is made at an f register's
 * first use.
 */
static uint32_t floc(struct gen *g, uint32_t r, int write)
{
	if (g->fmap[r] < 0)
		g->fmap[r] = (int8_t)g->nf++;
	if (write)
		g->fwritten |= 1u << r;
	return g->fbase + 1 + (uint32_t)g->fmap[r];
}

/* f[r] on the stack, as bits. */
static void freg_get(struct gen *g, uint32_t r)
{
	if (g->floc) {
		w_local(g->b, W_LOCAL_GET, floc(g, r, 0));
	} else {
		w_local(g->b, W_LOCAL_GET, L_CPU);
		w_mem(g->b, W_I64_LOAD, 3, offsetof(struct rv_cpu, f) + 8 * r);
	}
}

/* Storing into f[rd] the bits that freg_put_end() will find on the stack: this goes first. */
static void freg_put_begin(struct gen *g)
{
	if (!g->floc)
		w_local(g->b, W_LOCAL_GET, L_CPU);
}

static void freg_put_end(struct gen *g, uint32_t r)
{
	if (g->floc)
		w_local(g->b, W_LOCAL_SET, floc(g, r, 1));
	else
		w_mem(g->b, W_I64_STORE, 3, offsetof(struct rv_cpu, f) + 8 * r);
}

/* The bits of f[r] into a local. */
static void freg_to(struct gen *g, uint32_t r, uint32_t local)
{
	freg_get(g, r);
	w_local(g->b, W_LOCAL_SET, local);
}

/* The bits of the local into f[r]. */
static void freg_from(struct gen *g, uint32_t local, uint32_t r)
{
	freg_put_begin(g);
	w_local(g->b, W_LOCAL_GET, local);
	freg_put_end(g, r);
}

/* fcsr on the stack, as an i32. */
static void fcsr_get(struct gen *g)
{
	if (g->floc) {
		g->fc_used = 1;
		w_local(g->b, W_LOCAL_GET, g->fbase);
		w_op(g->b, W_I32_WRAP_I64);
	} else {
		w_local(g->b, W_LOCAL_GET, L_CPU);
		w_mem(g->b, W_I32_LOAD, 2, offsetof(struct rv_cpu, fcsr));
	}
}

/* Which f registers an instruction of F/D reads, and which it writes (bit masks); what is not known is taken as reading rs1, rs2 and writing rd. */
static void fp_regs(uint32_t i, uint32_t *src, uint32_t *dst)
{
	uint32_t s1 = 1u << RS1(i), s2 = 1u << RS2(i), d = 1u << RD(i);

	*src = *dst = 0;
	if (OPCODE(i) >= 0x43 && OPCODE(i) <= 0x4f) {
		*src = s1 | s2 | (1u << (i >> 27));
		*dst = d;
		return;
	}
	switch (i >> 27) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
		*src = s1 | s2; *dst = d; break;
	case 0x0b: case 0x08:
		*src = s1; *dst = d; break;
	case 0x14:
		*src = s1 | s2; break;
	case 0x18: case 0x1c:
		*src = s1; break;
	case 0x1a: case 0x1e:
		*dst = d; break;
	default:
		*src = s1 | s2; *dst = d; break;
	}
}

/*
 * A floating-point or CSR instruction by a call to the interpreter's code
 * for it. An integer result is left in L_TMP. `extra`: how many blocks the
 * caller has opened around this.
 */
static void gen_fp_call(struct gen *g, const struct jit_insn *insn, uint32_t not_run, uint32_t extra)
{
	struct wbuf *b = g->b;
	uint32_t i = insn->raw;
	int csr = OPCODE(i) == 0x73;
	int reads_int = csr ? !(F3(i) & 4) : rv_fp_int_source(i);

	uint32_t src = 0, dst = 0;

	/* The call sees *cpu: the operands and fcsr go there, and what it writes comes back. */
	if (g->floc && !csr) {
		fp_regs(i, &src, &dst);
		for (uint32_t r = 0; r < 32; r++) {
			if (!(src & (1u << r)))
				continue;
			w_local(b, W_LOCAL_GET, L_CPU);
			w_local(b, W_LOCAL_GET, floc(g, r, 0));
			w_mem(b, W_I64_STORE, 3, offsetof(struct rv_cpu, f) + 8 * r);
		}
		g->fc_used = g->fc_written = 1;
		w_local(b, W_LOCAL_GET, L_CPU);
		w_local(b, W_LOCAL_GET, g->fbase);
		w_op(b, W_I32_WRAP_I64);
		w_mem(b, W_I32_STORE, 2, offsetof(struct rv_cpu, fcsr));
	}
	w_local(b, W_LOCAL_GET, L_CPU);
	w_i64(b, i);
	if (reads_int)
		get_reg(g, RS1(i));
	else
		w_i64(b, 0);
	call_helper(g, csr ? H_CSR : H_FP);
	w_local(b, W_LOCAL_SET, L_TMP);

	/* Not a valid instruction after all (its rounding mode, say): the interpreter's to report. */
	w_local(b, W_LOCAL_GET, L_CPU);
	w_mem(b, W_I32_LOAD, 2, offsetof(struct rv_cpu, jit_fault));
	hint(g, HS_FAULT, 0);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	bail(g, insn, not_run, extra + 1);
	w_op(b, W_END);

	if (g->floc && !csr) {
		for (uint32_t r = 0; r < 32; r++) {
			if (!(dst & (1u << r)))
				continue;
			w_local(b, W_LOCAL_GET, L_CPU);
			w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, f) + 8 * r);
			w_local(b, W_LOCAL_SET, floc(g, r, 1));
		}
		w_local(b, W_LOCAL_GET, L_CPU);
		w_mem(b, W_I32_LOAD, 2, offsetof(struct rv_cpu, fcsr));
		w_op(b, W_I64_EXTEND_I32_U);
		w_local(b, W_LOCAL_SET, g->fbase);
	}
}

/* The double (or the single) whose bits are in a local, on the stack. */
static void push_double(struct gen *g, uint32_t local)
{
	w_local(g->b, W_LOCAL_GET, local);
	w_op(g->b, W_F64_REINTERPRET_I64);
}

static void push_single(struct gen *g, uint32_t local)
{
	w_local(g->b, W_LOCAL_GET, local);
	w_op(g->b, W_I32_WRAP_I64);
	w_op(g->b, W_F32_REINTERPRET_I32);
}

/* A single on the stack becomes the bits of the register that holds it (boxed), in L_FP. */
static void single_to_fp(struct gen *g)
{
	w_op(g->b, W_I32_REINTERPRET_F32);
	w_op(g->b, W_I64_EXTEND_I32_U);
	w_i64(g->b, 0xffffffff00000000ULL);
	w_op(g->b, W_I64_OR);
	w_local(g->b, W_LOCAL_SET, L_FP);
}

/* On the stack, as an i32 that is 0 or not: are the bits in the local those of a normal double? */
static void is_normal64(struct gen *g, uint32_t local)
{
	struct wbuf *b = g->b;

	/* The exponent is neither all zeros nor all ones: one more is neither 1 nor 0x800 (0, without the sign). */
	w_local(b, W_LOCAL_GET, local);
	w_i64(b, 52);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 1);
	w_op(b, W_I64_ADD);
	w_i64(b, 0x7fe);
	w_op(b, W_I64_AND);
	w_i64(b, 0);
	w_op(b, W_I64_NE);
}

/* ... those of a register holding a single (boxed)? */
static void is_boxed(struct gen *g, uint32_t local)
{
	w_local(g->b, W_LOCAL_GET, local);
	w_i64(g->b, 32);
	w_op(g->b, W_I64_SHR_U);
	w_i64(g->b, 0xffffffffULL);
	w_op(g->b, W_I64_EQ);
}

/* ... and that single a normal number? */
static void is_normal32(struct gen *g, uint32_t local)
{
	struct wbuf *b = g->b;

	w_local(b, W_LOCAL_GET, local);
	w_i64(b, 23);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 1);
	w_op(b, W_I64_ADD);
	w_i64(b, 0xfe);
	w_op(b, W_I64_AND);
	w_i64(b, 0);
	w_op(b, W_I64_NE);
	is_boxed(g, local);
	w_op(b, W_I32_AND);
}

/* ... a zero (of either sign)? A finite number? */
static void is_zero64(struct gen *g, uint32_t local)
{
	w_local(g->b, W_LOCAL_GET, local);
	w_i64(g->b, 1);
	w_op(g->b, W_I64_SHL);
	w_op(g->b, W_I64_EQZ);
}

static void is_finite64(struct gen *g, uint32_t local)
{
	struct wbuf *b = g->b;

	w_local(b, W_LOCAL_GET, local);
	w_i64(b, 52);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0x7ff);
	w_op(b, W_I64_AND);
	w_i64(b, 0x7ff);
	w_op(b, W_I64_NE);
}

/* The same for a register holding a single: boxed, and the single a zero, or finite. */
static void is_zero32(struct gen *g, uint32_t local)
{
	w_local(g->b, W_LOCAL_GET, local);
	w_i64(g->b, 33);
	w_op(g->b, W_I64_SHL);
	w_op(g->b, W_I64_EQZ);
	is_boxed(g, local);
	w_op(g->b, W_I32_AND);
}

static void is_finite32(struct gen *g, uint32_t local)
{
	struct wbuf *b = g->b;

	w_local(b, W_LOCAL_GET, local);
	w_i64(b, 23);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_i64(b, 0xff);
	w_op(b, W_I64_NE);
	is_boxed(g, local);
	w_op(b, W_I32_AND);
}

/*
 * On the stack: may an inexact result be the host's? The flag for inexact
 * results is set, and the rounding is to nearest (rm is the instruction's
 * field: 0 says so itself, 7 leaves it to fcsr).
 */
static void fcsr_allows(struct gen *g, uint32_t rm)
{
	fcsr_get(g);
	w_i32(g->b, rm == 7 ? 0xe1 : 0x01);
	w_op(g->b, W_I32_AND);
	w_i32(g->b, 1);
	w_op(g->b, W_I32_EQ);
}

/*
 * A floating-point instruction with the host's instructions, on the
 * conditions fp_fast() in fp.c has for it (often stricter ones: what is
 * rarer than it is worth testing for is left to the call). The pattern: the
 * candidate result is computed into L_FP, with a test of the conditions
 * left on the stack; if it holds the result is stored, otherwise the
 * instruction is done by the call. Returns 0 if this is not an instruction
 * for it, with nothing generated.
 */
static int gen_fp_host(struct gen *g, const struct jit_insn *insn, uint32_t not_run)
{
	struct wbuf *b = g->b;
	uint32_t i = insn->raw, rd = RD(i), rs1 = RS1(i), rs2 = RS2(i), f3 = F3(i), f7 = F7(i);
	uint32_t group = f7 >> 2;
	int dbl = f7 & 1, nearest = f3 == 0 || f3 == 7;
	int int_result = 0;

	if (f7 & 2)
		return 0;

	switch (group) {
	case 0x00: case 0x01: case 0x02: case 0x03: {
		/*
		 * FADD, FSUB, FMUL, FDIV. The result says enough about the
		 * operands: a sum that is finite had finite operands and raised no
		 * flag that was not set (a tiny sum is exact); a product or
		 * quotient that is normal, the same; a product that is zero is
		 * exact if an operand was zero (else it underflowed), and a
		 * quotient that is zero if the dividend was. Singles have to be
		 * boxed as well: an operand that is not is a NaN.
		 */
		void (*normal)(struct gen *, uint32_t) = dbl ? is_normal64 : is_normal32;
		void (*zero)(struct gen *, uint32_t) = dbl ? is_zero64 : is_zero32;

		if (!nearest)
			return 0;
		freg_to(g, rs1, L_ADDR);
		freg_to(g, rs2, L_TMP);
		if (dbl) {
			push_double(g, L_ADDR);
			push_double(g, L_TMP);
			w_op(b, (uint8_t)(W_F64_ADD + group));
			w_op(b, W_I64_REINTERPRET_F64);
			w_local(b, W_LOCAL_SET, L_FP);
		} else {
			push_single(g, L_ADDR);
			push_single(g, L_TMP);
			w_op(b, (uint8_t)(W_F32_ADD + group));
			single_to_fp(g);
		}
		fcsr_allows(g, f3);
		if (!dbl) {
			is_boxed(g, L_ADDR);
			w_op(b, W_I32_AND);
			is_boxed(g, L_TMP);
			w_op(b, W_I32_AND);
		}
		switch (group) {
		case 0x00: case 0x01:
			(dbl ? is_finite64 : is_finite32)(g, L_FP);
			break;
		case 0x02:
			normal(g, L_FP);
			zero(g, L_FP);
			zero(g, L_ADDR);
			zero(g, L_TMP);
			w_op(b, W_I32_OR);
			w_op(b, W_I32_AND);
			w_op(b, W_I32_OR);
			break;
		default:
			normal(g, L_FP);
			zero(g, L_FP);
			zero(g, L_ADDR);
			w_op(b, W_I32_AND);
			w_op(b, W_I32_OR);
		}
		w_op(b, W_I32_AND);
		break;
	}

	case 0x0b: /* FSQRT: of a positive normal number */
		if (!nearest || rs2 != 0)
			return 0;
		freg_to(g, rs1, L_ADDR);
		if (dbl) {
			push_double(g, L_ADDR);
			w_op(b, W_F64_SQRT);
			w_op(b, W_I64_REINTERPRET_F64);
			w_local(b, W_LOCAL_SET, L_FP);
			fcsr_allows(g, f3);
			/* Sign clear and the exponent neither all zeros nor all ones. */
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, 52);
			w_op(b, W_I64_SHR_U);
			w_i64(b, 1);
			w_op(b, W_I64_SUB);
			w_i64(b, 0x7fe);
			w_op(b, W_I64_LT_U);
			w_op(b, W_I32_AND);
		} else {
			push_single(g, L_ADDR);
			w_op(b, W_F32_SQRT);
			single_to_fp(g);
			fcsr_allows(g, f3);
			/* Boxed, sign clear, a normal number: all in one range of the bits above the fraction. */
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, 23);
			w_op(b, W_I64_SHR_U);
			w_i64(b, 0x1fffffe01ULL);
			w_op(b, W_I64_SUB);
			w_i64(b, 0xfe);
			w_op(b, W_I64_LT_U);
			w_op(b, W_I32_AND);
		}
		break;

	case 0x04: /* FSGNJ, FSGNJN, FSGNJX: bits */
		if (f3 > 2)
			return 0;
		freg_to(g, rs1, L_ADDR);
		freg_to(g, rs2, L_TMP);
		{
			uint64_t sign = dbl ? 0x8000000000000000ULL : 0x80000000ULL;

			if (f3 == 2) {
				w_local(b, W_LOCAL_GET, L_ADDR);
				w_local(b, W_LOCAL_GET, L_TMP);
				w_i64(b, sign);
				w_op(b, W_I64_AND);
				w_op(b, W_I64_XOR);
			} else {
				w_local(b, W_LOCAL_GET, L_ADDR);
				w_i64(b, ~sign);
				w_op(b, W_I64_AND);
				w_local(b, W_LOCAL_GET, L_TMP);
				if (f3 == 1) {
					w_i64(b, sign);
					w_op(b, W_I64_XOR);
				}
				w_i64(b, sign);
				w_op(b, W_I64_AND);
				w_op(b, W_I64_OR);
			}
			w_local(b, W_LOCAL_SET, L_FP);
		}
		if (dbl) {
			/* Always: no condition, no call. */
			freg_from(g, L_FP, rd);
			return 1;
		}
		is_boxed(g, L_ADDR);
		is_boxed(g, L_TMP);
		w_op(b, W_I32_AND);
		break;

	case 0x08: /* FCVT.S.D and FCVT.D.S */
		freg_to(g, rs1, L_ADDR);
		if (!dbl && rs2 == 1) {
			if (!nearest)
				return 0;
			push_double(g, L_ADDR);
			w_op(b, W_F32_DEMOTE_F64);
			single_to_fp(g);
			fcsr_allows(g, f3);
			is_normal64(g, L_ADDR);
			w_op(b, W_I32_AND);
			is_normal32(g, L_FP);
			w_op(b, W_I32_AND);
		} else if (dbl && rs2 == 0) {
			/* Exact for every single that is a number. */
			push_single(g, L_ADDR);
			w_op(b, W_F64_PROMOTE_F32);
			w_op(b, W_I64_REINTERPRET_F64);
			w_local(b, W_LOCAL_SET, L_FP);
			is_boxed(g, L_ADDR);
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_i64(b, 0x7f800000);
			w_op(b, W_I64_AND);
			w_i64(b, 0x7f800000);
			w_op(b, W_I64_NE);
			w_op(b, W_I32_AND);
		} else {
			return 0;
		}
		break;

	case 0x14: /* FLE, FLT, FEQ: of two numbers */
		if (f3 > 2)
			return 0;
		int_result = 1;
		freg_to(g, rs1, L_ADDR);
		freg_to(g, rs2, L_TMP);
		if (dbl) {
			push_double(g, L_ADDR);
			push_double(g, L_TMP);
			w_op(b, f3 == 0 ? W_F64_LE : f3 == 1 ? W_F64_LT : W_F64_EQ);
			w_op(b, W_I64_EXTEND_I32_U);
			w_local(b, W_LOCAL_SET, L_FP);
			push_double(g, L_ADDR);
			push_double(g, L_ADDR);
			w_op(b, W_F64_EQ);
			push_double(g, L_TMP);
			push_double(g, L_TMP);
			w_op(b, W_F64_EQ);
			w_op(b, W_I32_AND);
		} else {
			push_single(g, L_ADDR);
			push_single(g, L_TMP);
			w_op(b, f3 == 0 ? W_F32_LE : f3 == 1 ? W_F32_LT : W_F32_EQ);
			w_op(b, W_I64_EXTEND_I32_U);
			w_local(b, W_LOCAL_SET, L_FP);
			push_single(g, L_ADDR);
			push_single(g, L_ADDR);
			w_op(b, W_F32_EQ);
			push_single(g, L_TMP);
			push_single(g, L_TMP);
			w_op(b, W_F32_EQ);
			w_op(b, W_I32_AND);
			is_boxed(g, L_ADDR);
			w_op(b, W_I32_AND);
			is_boxed(g, L_TMP);
			w_op(b, W_I32_AND);
		}
		break;

	case 0x18: { /* FCVT.{W,WU,L,LU}.{S,D}: to an integer, a value that fits */
		static const double low[4] = { -2147483648.0, 0.0, -9223372036854775808.0, 0.0 };
		static const double high[4] = { 2147483647.0, 4294967295.0, 9223372036854775808.0, 18446744073709551616.0 };
		static const uint8_t convert[4] = {
			W_I32_TRUNC_SAT_F64_S, W_I32_TRUNC_SAT_F64_U, W_I64_TRUNC_SAT_F64_S, W_I64_TRUNC_SAT_F64_U,
		};
		/* The host rounds to a whole number the four ways a static rounding mode can ask for; dynamic: to nearest, if that is what fcsr says. */
		static const uint8_t whole[4] = { W_F64_NEAREST, W_F64_TRUNC, W_F64_FLOOR, W_F64_CEIL };

		if ((f3 > 3 && f3 != 7) || rs2 > 3)
			return 0;
		int_result = 1;
		freg_to(g, rs1, L_ADDR);
		/* The value without its fraction, as a double (its bits in L_TMP)... */
		if (dbl) {
			push_double(g, L_ADDR);
		} else {
			push_single(g, L_ADDR);
			w_op(b, W_F64_PROMOTE_F32);
		}
		w_op(b, whole[f3 == 7 ? 0 : f3]);
		w_op(b, W_I64_REINTERPRET_F64);
		w_local(b, W_LOCAL_SET, L_TMP);
		/* ... as the integer (32-bit results are sign-extended, the unsigned one too)... */
		push_double(g, L_TMP);
		w_byte(b, W_MISC_PREFIX);
		w_uleb(b, convert[rs2]);
		if (rs2 < 2)
			w_op(b, W_I64_EXTEND_I32_S);
		w_local(b, W_LOCAL_SET, L_FP);
		/* ... if it is in range (a NaN is in no range), the flag for inexact results is set, and the rounding is as above. */
		fcsr_allows(g, f3 == 7 ? 7 : 0);
		push_double(g, L_TMP);
		w_f64(b, low[rs2]);
		w_op(b, W_F64_GE);
		w_op(b, W_I32_AND);
		push_double(g, L_TMP);
		w_f64(b, high[rs2]);
		w_op(b, rs2 < 2 ? W_F64_LE : W_F64_LT);
		w_op(b, W_I32_AND);
		if (!dbl) {
			is_boxed(g, L_ADDR);
			w_op(b, W_I32_AND);
		}
		break;
	}

	case 0x1a: /* FCVT.{S,D}.{W,WU,L,LU}: from an integer */
		if (rs2 > 3)
			return 0;
		if (dbl && rs2 < 2) {
			/* A 32-bit integer is a double exactly: always. */
			freg_put_begin(g);
			get_reg(g, rs1);
			w_op(b, W_I32_WRAP_I64);
			w_op(b, rs2 ? W_F64_CONVERT_I32_U : W_F64_CONVERT_I32_S);
			w_op(b, W_I64_REINTERPRET_F64);
			freg_put_end(g, rd);
			return 1;
		}
		/*
		 * Otherwise the host's conversion is right when it is exact (an
		 * integer of at most 53, or 24, significant bits: then the
		 * rounding mode does not matter), or when rounding to nearest
		 * and the inexact flag is set already.
		 */
		get_reg(g, rs1);
		w_local(b, W_LOCAL_SET, L_TMP);
		w_local(b, W_LOCAL_GET, L_TMP);
		if (dbl) {
			w_op(b, rs2 == 2 ? W_F64_CONVERT_I64_S : W_F64_CONVERT_I64_U);
			w_op(b, W_I64_REINTERPRET_F64);
			w_local(b, W_LOCAL_SET, L_FP);
		} else {
			static const uint8_t convert[4] = {
				W_F32_CONVERT_I32_S, W_F32_CONVERT_I32_U, W_F32_CONVERT_I64_S, W_F32_CONVERT_I64_U,
			};

			if (rs2 < 2)
				w_op(b, W_I32_WRAP_I64);
			w_op(b, convert[rs2]);
			single_to_fp(g);
		}
		{
			uint64_t digits = dbl ? 53 : 24;

			w_local(b, W_LOCAL_GET, L_TMP);
			if (!dbl && rs2 < 2)
				w_op(b, W_I32_WRAP_I64);
			if (rs2 & 1) {
				/* Unsigned: at most 2^digits. */
				if (!dbl && rs2 < 2) {
					w_i32(b, (1u << digits) + 1);
					w_op(b, W_I32_LT_U);
				} else {
					w_i64(b, (1ULL << digits) + 1);
					w_op(b, W_I64_LT_U);
				}
			} else {
				/* Signed: within 2^digits either way. */
				if (!dbl && rs2 < 2) {
					w_i32(b, 1u << digits);
					w_op(b, W_I32_ADD);
					w_i32(b, (1u << (digits + 1)) + 1);
					w_op(b, W_I32_LT_U);
				} else {
					w_i64(b, 1ULL << digits);
					w_op(b, W_I64_ADD);
					w_i64(b, (1ULL << (digits + 1)) + 1);
					w_op(b, W_I64_LT_U);
				}
			}
		}
		if (nearest) {
			fcsr_allows(g, f3);
			w_op(b, W_I32_OR);
		}
		break;

	case 0x1c: /* FMV.X.W and FMV.X.D: bits (FCLASS is the call's) */
		if (rs2 != 0 || f3 != 0)
			return 0;
		freg_get(g, rs1);
		if (!dbl)
			sext32(g);
		set_reg(g, rd);
		return 1;

	case 0x1e: /* FMV.W.X and FMV.D.X */
		if (rs2 != 0 || f3 != 0)
			return 0;
		freg_put_begin(g);
		get_reg(g, rs1);
		if (!dbl) {
			w_i64(b, 0xffffffffULL);
			w_op(b, W_I64_AND);
			w_i64(b, 0xffffffff00000000ULL);
			w_op(b, W_I64_OR);
		}
		freg_put_end(g, rd);
		return 1;

	default:
		return 0;
	}

	/* The test is on the stack and the candidate in L_FP. */
	hint(g, HS_FP, 1);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	if (int_result) {
		w_local(b, W_LOCAL_GET, L_FP);
		w_local(b, W_LOCAL_SET, L_TMP);
	} else {
		freg_from(g, L_FP, rd);
	}
	w_byte(b, W_ELSE);
	gen_fp_call(g, insn, not_run, 1);
	w_op(b, W_END);
	if (int_result) {
		w_local(b, W_LOCAL_GET, L_TMP);
		set_reg(g, rd);
	}
	return 1;
}

/* On the stack: is the exponent of the double in the local safely in the middle (moderate64() in fp.c)? */
static void is_moderate64(struct gen *g, uint32_t local)
{
	struct wbuf *b = g->b;

	w_local(b, W_LOCAL_GET, local);
	w_i64(b, 52);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0x7ff);
	w_op(b, W_I64_AND);
	w_i64(b, 0x3ff - 400);
	w_op(b, W_I64_SUB);
	w_i64(b, 801);
	w_op(b, W_I64_LT_U);
}

/*
 * Does the host multiply and add in one rounding? Wasm has an instruction
 * that may: f64x2.relaxed_madd is a fused multiply-add where the processor
 * has one and the browser uses it (Chrome does, on arm64 and on x86 with
 * FMA3), and a product rounded and then a sum rounded where not; which of
 * the two is the same for as long as the page lives. So it is tried: a small
 * function of its own that does nothing else, on operands for which the two
 * ways differ and on others, against the emulator's own exact computation.
 * All the same: the instruction is what FMADD is, on the terms that
 * rv_jit_fused64() is (gen_fp_fused()). A host that cannot compile the
 * function has no such instruction. (Compilers for RISC-V fuse wherever the
 * source allows it, so numeric code is full of these: of the first 400
 * instructions of the sine that Node's Math.sin is, 62.)
 */
static int fma_probe(struct rv_jit *jit)
{
#ifndef __wasm__
	if (!jit->emu)
		return -1;
#endif
	static const uint8_t head[] = {
		0, 'a', 's', 'm', 1, 0, 0, 0,
		1, 7, 1, W_TYPE_FUNC, 3, W_TYPE_I64, W_TYPE_I32, W_TYPE_I32, 0,	/* the type of a region */
		2, 18, 1, 3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 7, 0, 0x80, 0x80, 0x10,
		3, 2, 1, 0,
		7, 5, 1, 1, 'r', 0, 0,
	};
	uint8_t data[160], body_data[64];
	struct wbuf m = { data, 0, sizeof(data), 0 }, body = { body_data, 0, sizeof(body_data), 0 };
	uint64_t v[4], seed = 0x9e3779b97f4a7c15ULL;
	long slot;

	if (jit->off & (OFF_FMA | OFF_FPHOST))
		return -1;
	/* (cpu, entry, budget): at `cpu` three doubles a, b, c; a * b + c goes after them. */
	w_uleb(&body, 0);
	w_local(&body, W_LOCAL_GET, 0);
	for (uint64_t k = 0; k < 3; k++) {
		w_local(&body, W_LOCAL_GET, 0);
		w_mem(&body, W_I64_LOAD, 3, 8 * k);
		w_simd(&body, W_I64X2_SPLAT);
	}
	w_simd(&body, W_F64X2_RELAXED_MADD);
	w_simd(&body, W_I64X2_EXTRACT_LANE);
	w_byte(&body, 0);
	w_mem(&body, W_I64_STORE, 3, 24);
	w_op(&body, W_END);
	w_bytes(&m, head, sizeof(head));
	w_byte(&m, 10);
	w_uleb(&m, body.len + 2);
	w_uleb(&m, 1);
	w_uleb(&m, body.len);
	w_bytes(&m, body.data, body.len);
#ifdef __wasm__
	if (m.overflow || body.overflow || (slot = __rv_jit_install(m.data, m.len, -1)) < 0)
		return -1;
#else
	if (m.overflow || body.overflow || (slot = rv_emu_install(m.data, m.len, -1)) < 0)
		return -1;
#endif

	for (int k = 0; k < 64; k++) {
		if (k == 0) {
			/* (1 + 2^-27)^2 - (1 + 2^-26) is 2^-54, or 0 if the product was rounded first. */
			v[0] = v[1] = 0x3ff0000002000000ULL;
			v[2] = 0xbff0000004000000ULL;
		} else if (k == 1) {
			v[0] = 0x4008000000000000ULL;	/* 3 * 5 - 15: a zero, and which */
			v[1] = 0x4014000000000000ULL;
			v[2] = 0xc02e000000000000ULL;
		} else {
			/* Any fractions, signs and exponents that rv_jit_fused64() is for. */
			for (int j = 0; j < 3; j++) {
				seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
				v[j] = (seed & 0x800fffffffffffffULL) | (0x3ffULL - 60 + ((seed >> 52) % 120)) << 52;
			}
			if (k & 1)	/* a sum that nearly cancels */
				v[2] = rv_jit_fused64(v[0], v[1], 0) ^ 0x8000000000000001ULL;
		}
		v[3] = 0;
		region_call_raw(jit, (uint32_t)slot, (struct rv_cpu *)(void *)v, 0, 0);
		if (v[3] != rv_jit_fused64(v[0], v[1], v[2]))
			return -1;
	}
	return 1;
}

/*
 * The fused instructions on doubles: a * b + c in one rounding. By the
 * host's instruction for that if it has one (fma_probe()), else by the
 * emulator's exact way of computing it (fused64() in fp.c), called directly;
 * either when the operands are such that the latter is exact (tested here,
 * as in fused_fast()), and stored when the result is a normal number or
 * zero. Otherwise, and for singles, the general call.
 */
static int gen_fp_fused(struct gen *g, const struct jit_insn *insn, uint32_t not_run)
{
	struct wbuf *b = g->b;
	uint32_t i = insn->raw, rd = RD(i), f3 = F3(i), opcode = OPCODE(i);
	const uint64_t sign = 0x8000000000000000ULL;
	int fma;

	if (((i >> 25) & 3) != 1 || !(f3 == 0 || f3 == 7))
		return 0;
	if (!g->cpu->jit->fma)
		g->cpu->jit->fma = fma_probe(g->cpu->jit);
	fma = g->cpu->jit->fma > 0;

	freg_to(g, RS1(i), L_ADDR);
	freg_to(g, RS2(i), L_TMP);
	freg_to(g, i >> 27, L_FP);

	/* L_TMP32: whether the result in L_FP is to be stored. */
	w_i32(b, 0);
	w_local(b, W_LOCAL_SET, L_TMP32);
	fcsr_allows(g, f3);
	is_moderate64(g, L_ADDR);
	w_op(b, W_I32_AND);
	is_moderate64(g, L_TMP);
	w_op(b, W_I32_AND);
	is_moderate64(g, L_FP);
	w_local(b, W_LOCAL_GET, L_FP);
	w_i64(b, 1);
	w_op(b, W_I64_SHL);
	w_op(b, W_I64_EQZ);
	w_op(b, W_I32_OR);
	w_op(b, W_I32_AND);
	hint(g, HS_FP, 1);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_local(b, W_LOCAL_GET, L_ADDR);
	if (opcode == 0x4b || opcode == 0x4f) {	/* minus the product */
		w_i64(b, sign);
		w_op(b, W_I64_XOR);
	}
	if (fma)
		w_simd(b, W_I64X2_SPLAT);
	w_local(b, W_LOCAL_GET, L_TMP);
	if (fma)
		w_simd(b, W_I64X2_SPLAT);
	w_local(b, W_LOCAL_GET, L_FP);
	if (opcode == 0x47 || opcode == 0x4f) {	/* minus the third operand */
		w_i64(b, sign);
		w_op(b, W_I64_XOR);
	}
	if (fma) {
		w_simd(b, W_I64X2_SPLAT);
		w_simd(b, W_F64X2_RELAXED_MADD);
		w_simd(b, W_I64X2_EXTRACT_LANE);
		w_byte(b, 0);
	} else {
		call_helper(g, H_FUSED64);
	}
	w_local(b, W_LOCAL_SET, L_FP);
	is_normal64(g, L_FP);
	w_local(b, W_LOCAL_GET, L_FP);
	w_i64(b, 1);
	w_op(b, W_I64_SHL);
	w_op(b, W_I64_EQZ);
	w_op(b, W_I32_OR);
	w_local(b, W_LOCAL_SET, L_TMP32);
	w_op(b, W_END);

	w_local(b, W_LOCAL_GET, L_TMP32);
	hint(g, HS_FP, 1);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	freg_from(g, L_FP, rd);
	w_byte(b, W_ELSE);
	gen_fp_call(g, insn, not_run, 1);
	w_op(b, W_END);
	return 1;
}

/*
 * The CSRs of floating point (fflags, frm, fcsr), read and written in place:
 * bits of cpu->fcsr, as rv_csr() in fp.c has them. A JavaScript engine's
 * generated code reads and clears the flags around every conversion to an
 * integer, which made the call for it a tenth of a numeric benchmark. The
 * counters are left to the call. Returns 0 if this is not such an
 * instruction, with nothing generated.
 */
static int gen_csr(struct gen *g, const struct jit_insn *insn)
{
	struct wbuf *b = g->b;
	uint32_t i = insn->raw, csr = i >> 20, f3 = F3(i), op = f3 & 3, rd = RD(i), rs1 = RS1(i);
	int writes = op == 1 || rs1 != 0;

	if (op == 0 || csr < 1 || csr > 3)
		return 0;
	/* The whole register in L_TMP, the CSR as it was in L_FP. */
	fcsr_get(g);
	w_op(b, W_I64_EXTEND_I32_U);
	w_local(b, W_LOCAL_TEE, L_TMP);
	if (csr == 2) {
		w_i64(b, 5);
		w_op(b, W_I64_SHR_U);
	}
	w_i64(b, csr == 1 ? 0x1f : csr == 2 ? 7 : 0xff);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_SET, L_FP);
	if (writes) {
		/* The new value of the CSR: the source, or the old value with the source's bits set or cleared, in L_HOST... */
		if (op != 1)
			w_local(b, W_LOCAL_GET, L_FP);
		if (f3 & 4)
			w_i64(b, rs1);
		else
			get_reg(g, rs1);
		if (op == 3) {
			w_i64(b, -1);
			w_op(b, W_I64_XOR);
			w_op(b, W_I64_AND);
		} else if (op == 2) {
			w_op(b, W_I64_OR);
		}
		w_i64(b, csr == 1 ? 0x1f : csr == 2 ? 7 : 0xff);
		w_op(b, W_I64_AND);
		if (csr == 2) {
			w_i64(b, 5);
			w_op(b, W_I64_SHL);
		}
		w_local(b, W_LOCAL_SET, L_HOST);
		/* ... put into the register in place of what was there. */
		if (g->floc) {
			g->fc_used = g->fc_written = 1;
			w_local(b, W_LOCAL_GET, L_TMP);
			w_i64(b, csr == 1 ? ~0x1fULL : csr == 2 ? 0x1f : 0);
			w_op(b, W_I64_AND);
			w_local(b, W_LOCAL_GET, L_HOST);
			w_op(b, W_I64_OR);
			w_i64(b, 0xffffffffULL);
			w_op(b, W_I64_AND);
			w_local(b, W_LOCAL_SET, g->fbase);
		} else {
			w_local(b, W_LOCAL_GET, L_CPU);
			w_local(b, W_LOCAL_GET, L_TMP);
			w_i64(b, csr == 1 ? ~0x1fULL : csr == 2 ? 0x1f : 0);
			w_op(b, W_I64_AND);
			w_local(b, W_LOCAL_GET, L_HOST);
			w_op(b, W_I64_OR);
			w_op(b, W_I32_WRAP_I64);
			w_mem(b, W_I32_STORE, 2, offsetof(struct rv_cpu, fcsr));
		}
	}
	w_local(b, W_LOCAL_GET, L_FP);
	set_reg(g, rd);
	return 1;
}

/*
 * Zba, Zbb and Zbs (bitmanip.c): Wasm has most of them as one instruction or
 * a few. Returns 0, with nothing generated, for anything else.
 */
static int gen_bitmanip(struct gen *g, uint32_t i)
{
	struct wbuf *b = g->b;
	uint32_t rs1 = RS1(i), rs2 = RS2(i), shamt = (i >> 20) & 63;
	int kind = rv_zb_kind(i);

	if (!kind)
		return 0;
	if (rv_zb_counting > 0) {
		/* RV_ZB_COUNT: one more in the table, at its address in memory. */
		uint64_t at = (uint64_t)(uintptr_t)&rv_zb_count[kind];

		w_i64(b, 0);
		w_i64(b, 0);
		w_mem(b, W_I64_LOAD, 3, at);
		w_i64(b, 1);
		w_op(b, W_I64_ADD);
		w_mem(b, W_I64_STORE, 3, at);
	}
	/* Every one has rs1 as its first operand: on the stack from here on (but for select's order, below). */
	if (kind < RV_ZB_MAX || kind > RV_ZB_MINU)
		get_reg(g, rs1);
	switch (kind) {
	case RV_ZB_ADD_UW: case RV_ZB_SH1ADD_UW: case RV_ZB_SH2ADD_UW: case RV_ZB_SH3ADD_UW:
		w_op(b, W_I32_WRAP_I64);		/* .uw: rs1's low 32 bits, zero-extended */
		w_op(b, W_I64_EXTEND_I32_U);
		/* fall through */
	case RV_ZB_SH1ADD: case RV_ZB_SH2ADD: case RV_ZB_SH3ADD: {
		static const uint8_t shift[RV_ZB_KINDS] = {
			[RV_ZB_SH1ADD] = 1, [RV_ZB_SH2ADD] = 2, [RV_ZB_SH3ADD] = 3,
			[RV_ZB_SH1ADD_UW] = 1, [RV_ZB_SH2ADD_UW] = 2, [RV_ZB_SH3ADD_UW] = 3,
		};

		if (shift[kind]) {
			w_i64(b, shift[kind]);
			w_op(b, W_I64_SHL);
		}
		get_reg(g, rs2);
		w_op(b, W_I64_ADD);
		break;
	}
	case RV_ZB_SLLI_UW:
		w_op(b, W_I32_WRAP_I64);
		w_op(b, W_I64_EXTEND_I32_U);
		w_i64(b, shamt);
		w_op(b, W_I64_SHL);
		break;
	case RV_ZB_ANDN: case RV_ZB_ORN:
		get_reg(g, rs2);
		w_i64(b, ~0ULL);
		w_op(b, W_I64_XOR);
		w_op(b, kind == RV_ZB_ANDN ? W_I64_AND : W_I64_OR);
		break;
	case RV_ZB_XNOR:
		get_reg(g, rs2);
		w_op(b, W_I64_XOR);
		w_i64(b, ~0ULL);
		w_op(b, W_I64_XOR);
		break;
	case RV_ZB_CLZ: w_op(b, W_I64_CLZ); break;
	case RV_ZB_CTZ: w_op(b, W_I64_CTZ); break;
	case RV_ZB_CPOP: w_op(b, W_I64_POPCNT); break;
	case RV_ZB_CLZW: case RV_ZB_CTZW: case RV_ZB_CPOPW:
		w_op(b, W_I32_WRAP_I64);
		w_op(b, kind == RV_ZB_CLZW ? W_I32_CLZ : kind == RV_ZB_CTZW ? W_I32_CTZ : W_I32_POPCNT);
		w_op(b, W_I64_EXTEND_I32_U);
		break;
	case RV_ZB_MIN: case RV_ZB_MINU: case RV_ZB_MAX: case RV_ZB_MAXU: {
		/* select(first, second, rs1 < rs2): min takes rs1 when it is less, max rs2. */
		int max = kind == RV_ZB_MAX || kind == RV_ZB_MAXU;

		get_reg(g, max ? rs2 : rs1);
		get_reg(g, max ? rs1 : rs2);
		get_reg(g, rs1);
		get_reg(g, rs2);
		w_op(b, kind == RV_ZB_MIN || kind == RV_ZB_MAX ? W_I64_LT_S : W_I64_LT_U);
		w_op(b, W_SELECT);
		break;
	}
	case RV_ZB_SEXT_B: w_op(b, W_I64_EXTEND8_S); break;
	case RV_ZB_SEXT_H: w_op(b, W_I64_EXTEND16_S); break;
	case RV_ZB_ZEXT_H:
		w_i64(b, 0xffff);
		w_op(b, W_I64_AND);
		break;
	case RV_ZB_ROL: case RV_ZB_ROR:	/* Wasm's rotations take the amount modulo the width too */
		get_reg(g, rs2);
		w_op(b, kind == RV_ZB_ROL ? W_I64_ROTL : W_I64_ROTR);
		break;
	case RV_ZB_RORI:
		w_i64(b, shamt);
		w_op(b, W_I64_ROTR);
		break;
	case RV_ZB_ROLW: case RV_ZB_RORW: case RV_ZB_RORIW:
		w_op(b, W_I32_WRAP_I64);
		if (kind == RV_ZB_RORIW) {
			w_i32(b, (int32_t)(shamt & 31));
		} else {
			get_reg(g, rs2);
			w_op(b, W_I32_WRAP_I64);
		}
		w_op(b, kind == RV_ZB_ROLW ? W_I32_ROTL : W_I32_ROTR);
		w_op(b, W_I64_EXTEND_I32_S);
		break;
	case RV_ZB_ORC_B:
		/*
		 * Per byte: (its low seven bits + 0x7f) | itself has the top bit
		 * set if and only if the byte is not zero, and carries into no
		 * other byte. That bit, moved to the bottom, times 0xff.
		 */
		w_i64(b, 0x7f7f7f7f7f7f7f7fULL);
		w_op(b, W_I64_AND);
		w_i64(b, 0x7f7f7f7f7f7f7f7fULL);
		w_op(b, W_I64_ADD);
		get_reg(g, rs1);
		w_op(b, W_I64_OR);
		w_i64(b, 7);
		w_op(b, W_I64_SHR_U);
		w_i64(b, 0x0101010101010101ULL);
		w_op(b, W_I64_AND);
		w_i64(b, 0xff);
		w_op(b, W_I64_MUL);
		break;
	case RV_ZB_REV8: {
		/* Swap the bytes of each pair, then the pairs of each half, then the halves. */
		static const uint64_t mask[2] = { 0x00ff00ff00ff00ffULL, 0x0000ffff0000ffffULL };

		for (int k = 0; k < 2; k++) {
			w_local(b, W_LOCAL_TEE, L_TMP);
			w_i64(b, 8 << k);
			w_op(b, W_I64_SHR_U);
			w_i64(b, mask[k]);
			w_op(b, W_I64_AND);
			w_local(b, W_LOCAL_GET, L_TMP);
			w_i64(b, mask[k]);
			w_op(b, W_I64_AND);
			w_i64(b, 8 << k);
			w_op(b, W_I64_SHL);
			w_op(b, W_I64_OR);
		}
		w_i64(b, 32);
		w_op(b, W_I64_ROTL);
		break;
	}
	case RV_ZB_BCLR: case RV_ZB_BINV: case RV_ZB_BSET:
		w_i64(b, 1);
		get_reg(g, rs2);
		w_op(b, W_I64_SHL);
		if (kind == RV_ZB_BCLR) {
			w_i64(b, ~0ULL);
			w_op(b, W_I64_XOR);
		}
		w_op(b, kind == RV_ZB_BCLR ? W_I64_AND : kind == RV_ZB_BINV ? W_I64_XOR : W_I64_OR);
		break;
	case RV_ZB_BCLRI: w_i64(b, ~(1ULL << shamt)); w_op(b, W_I64_AND); break;
	case RV_ZB_BINVI: w_i64(b, 1ULL << shamt); w_op(b, W_I64_XOR); break;
	case RV_ZB_BSETI: w_i64(b, 1ULL << shamt); w_op(b, W_I64_OR); break;
	case RV_ZB_BEXT: case RV_ZB_BEXTI:
		if (kind == RV_ZB_BEXT)
			get_reg(g, rs2);
		else
			w_i64(b, shamt);
		w_op(b, W_I64_SHR_U);
		w_i64(b, 1);
		w_op(b, W_I64_AND);
		break;
	}
	set_reg(g, RD(i));
	return 1;
}

/*
 * DIV, DIVU, REM, REMU and their W forms, inline: Wasm's divisions trap where
 * RISC-V's have results (a divisor of 0, and the most negative dividend over
 * -1 for a signed quotient), so the divisor is made 1 in those cases and the
 * RISC-V result selected afterwards: by 0, the quotient is all ones and the
 * remainder the dividend; overflowing, the quotient is the dividend (0 - a,
 * which wraps to it) and the remainder 0 (which Wasm's rem_s gives without
 * trapping). The W forms are done in 64 bits on the operands' low halves,
 * sign- or zero-extended: no 64-bit case can then overflow, and the low 32
 * bits of the result are RISC-V's. A call to the emulator for each was
 * dearer than the division.
 */
static void gen_divrem(struct gen *g, uint32_t f3, uint32_t rs1, uint32_t rs2, int word)
{
	struct wbuf *b = g->b;
	int rem = f3 >= 6, sign = !(f3 & 1);

	/* a in L_ADDR, b in L_TMP, as 64-bit values */
	for (int k = 0; k < 2; k++) {
		get_reg(g, k ? rs2 : rs1);
		if (word) {
			if (sign) {
				w_op(b, W_I32_WRAP_I64);
				w_op(b, W_I64_EXTEND_I32_S);
			} else {
				w_i64(b, 0xffffffffULL);
				w_op(b, W_I64_AND);
			}
		}
		w_local(b, W_LOCAL_SET, k ? L_TMP : L_ADDR);
	}
	/* L_TMP32: the divisor is special (0; or -1 for a signed 64-bit quotient) */
	w_local(b, W_LOCAL_GET, L_TMP);
	if (sign && !rem && !word) {
		w_i64(b, 1);
		w_op(b, W_I64_ADD);
		w_i64(b, 2);
		w_op(b, W_I64_LT_U);
	} else {
		w_op(b, W_I64_EQZ);
	}
	w_local(b, W_LOCAL_SET, L_TMP32);
	/* The result if special: quotient -1 (by 0) or -a (overflow); remainder a (by 0)... */
	if (rem) {
		w_local(b, W_LOCAL_GET, L_ADDR);
	} else {
		w_i64(b, (uint64_t)-1);
		w_i64(b, 0);
		w_local(b, W_LOCAL_GET, L_ADDR);
		w_op(b, W_I64_SUB);
		w_local(b, W_LOCAL_GET, L_TMP);
		w_op(b, W_I64_EQZ);
		w_op(b, W_SELECT);
	}
	/* ... else the division, by the divisor made 1 when special. */
	w_local(b, W_LOCAL_GET, L_ADDR);
	w_i64(b, 1);
	w_local(b, W_LOCAL_GET, L_TMP);
	w_local(b, W_LOCAL_GET, L_TMP32);
	w_op(b, W_SELECT);
	w_op(b, rem ? (sign ? W_I64_REM_S : W_I64_REM_U) : (sign ? W_I64_DIV_S : W_I64_DIV_U));
	w_local(b, W_LOCAL_GET, L_TMP32);
	w_op(b, W_SELECT);
	if (word)
		sext32(g);
}

static void gen_insn(struct gen *g, const struct jit_insn *insn, uint32_t not_run)
{
	struct wbuf *b = g->b;
	uint32_t i = insn->raw, rd = RD(i), rs1 = RS1(i), rs2 = RS2(i), f3 = F3(i), f7 = F7(i);
	uint64_t pc = insn->pc, next = pc + insn->len;

	if (gen_bitmanip(g, i))
		return;
	switch (OPCODE(i)) {
	case 0x37: /* LUI */
		w_i64(b, (uint64_t)IMM_U(i));
		set_reg(g, rd);
		break;

	case 0x17: /* AUIPC */
		w_i64(b, pc + (uint64_t)IMM_U(i));
		set_reg(g, rd);
		break;

	case 0x6f: /* JAL */
		w_i64(b, next);
		set_reg(g, rd);
		transition(g, pc + (uint64_t)imm_j(i), 0);
		break;

	case 0x67: /* JALR: the target is only known now */
		get_reg(g, rs1);
		w_i64(b, (uint64_t)IMM_I(i));
		w_op(b, W_I64_ADD);
		w_i64(b, ~1ULL);
		w_op(b, W_I64_AND);
		w_local(b, W_LOCAL_SET, L_PC);	/* before rd is written: rd may be rs1 */
		w_i64(b, next);
		set_reg(g, rd);

		/*
		 * Most indirect jumps are returns, and most returns go to a block
		 * of this same region: go on at the region's lookup of its jump
		 * targets (gen_lookup()), the last thing the loop dispatches to,
		 * which goes on at the block or leaves.
		 */
		if (g->cpu->jit->targets_now) {
			uint64_t targets[JALR_TARGETS];
			int n = jalr_targets(g->cpu->jit, pc, targets), scope = g->scope;

			/* The targets seen while interpreting, each compared with inline, if there are few. */
			if (n <= JALR_INLINE) {
				for (int k = 0; k < n; k++) {
					int t = find_block(g->r, targets[k]);

					if (t < 0)
						continue;
					w_local(b, W_LOCAL_GET, L_PC);
					w_i64(b, targets[k]);
					w_op(b, W_I64_EQ);
					w_byte(b, W_IF);
					w_byte(b, W_TYPE_VOID);
					branch_to_block(g, t, 1);
					w_op(b, W_END);
				}
			}
			/* Then the lookup: this loop's, if it has one, else the region's. */
			while (scope > 0 && g->lay->scopes[scope].lookup < 0)
				scope = g->lay->scopes[scope].parent;
			if (scope > 0 || g->lay->scopes[0].lookup >= 0) {
				set_next(g, g->r->nblocks + scope);
				w_br(b, W_BR, depth_to_loop(g, scope, 0));
				break;
			}
		}
		leave_for_target(g, 0);
		break;

	case 0x63: { /* BRANCH */
		static const uint8_t cond[8] = {
			W_I64_EQ, W_I64_NE, 0, 0, W_I64_LT_S, W_I64_GE_S, W_I64_LT_U, W_I64_GE_U,
		};

		get_reg(g, rs1);
		get_reg(g, rs2);
		w_op(b, cond[f3]);
		w_byte(b, W_IF);
		w_byte(b, W_TYPE_VOID);
		transition(g, pc + (uint64_t)imm_b(i), 1);
		w_op(b, W_END);
		transition(g, next, 0);
		break;
	}

	case 0x03: { /* LOAD */
		static const uint8_t op[7] = {
			W_I64_LOAD8_S, W_I64_LOAD16_S, W_I64_LOAD32_S, W_I64_LOAD,
			W_I64_LOAD8_U, W_I64_LOAD16_U, W_I64_LOAD32_U,
		};

		effective_address(g, rs1, IMM_I(i));
		check_access(g, insn, not_run, 1u << (f3 & 3), RV_PROT_READ, rs1);
		w_local(b, W_LOCAL_GET, L_HOST);
		w_mem(b, op[f3], 0, 0);
		set_reg(g, rd);
		break;
	}

	case 0x23: { /* STORE */
		static const uint8_t op[4] = { W_I64_STORE8, W_I64_STORE16, W_I64_STORE32, W_I64_STORE };

		effective_address(g, rs1, IMM_S(i));
		check_access(g, insn, not_run, 1u << f3, RV_PROT_WRITE, rs1);
		w_local(b, W_LOCAL_GET, L_HOST);
		get_reg(g, rs2);
		w_mem(b, op[f3], 0, 0);
		break;
	}

	case 0x13: /* OP-IMM */
		get_reg(g, rs1);
		if (f3 == 1 || f3 == 5) {
			w_i64(b, (i >> 20) & 63);
			alu(g, f3, (i >> 26) == 0x10, 0);
		} else {
			w_i64(b, (uint64_t)IMM_I(i));
			alu(g, f3, 0, 0);
		}
		set_reg(g, rd);
		break;

	case 0x1b: /* OP-IMM-32 */
		get_reg(g, rs1);
		if (f3 == 0)
			w_i64(b, (uint64_t)IMM_I(i));
		else
			w_i64(b, rs2);	/* the shift amount */
		/*
		 * Bit 30 tells SRAIW from SRLIW. In ADDIW those bits belong to the
		 * immediate: taking them for "subtract" turned addiw x, x, 0x401
		 * into a subtraction (perl's hash function was the first to care).
		 */
		alu(g, f3, f3 == 5 && f7 == 0x20, 1);
		set_reg(g, rd);
		break;

	case 0x33: /* OP */
		if (f7 == 1 && f3 >= 4 && !(g->cpu->jit->off & OFF_DIV)) {
			gen_divrem(g, f3, rs1, rs2, 0);
			set_reg(g, rd);
			break;
		}
		get_reg(g, rs1);
		get_reg(g, rs2);
		if (f7 == 1) {
			static const int8_t helper[8] = {
				-1, H_MULH, H_MULHSU, H_MULHU, H_DIV, H_DIVU, H_REM, H_REMU,
			};

			if (f3 == 0)
				w_op(b, W_I64_MUL);
			else
				call_helper(g, helper[f3]);
		} else {
			alu(g, f3, f7 == 0x20, 0);
		}
		set_reg(g, rd);
		break;

	case 0x3b: /* OP-32 */
		if (f7 == 1 && f3 >= 4 && !(g->cpu->jit->off & OFF_DIV)) {
			gen_divrem(g, f3, rs1, rs2, 1);
			set_reg(g, rd);
			break;
		}
		get_reg(g, rs1);
		get_reg(g, rs2);
		if (f7 == 1) {
			static const int8_t helper[8] = {
				-1, -1, -1, -1, H_DIVW, H_DIVUW, H_REMW, H_REMUW,
			};

			if (f3 == 0) {
				w_op(b, W_I64_MUL);
				sext32(g);
			} else {
				call_helper(g, helper[f3]);
			}
		} else {
			alu(g, f3, f7 == 0x20, 1);
		}
		set_reg(g, rd);
		break;

	case 0x0f: /* FENCE: the host's memory order is no weaker than the guest's instructions assume */
		break;

	case 0x2f: { /* The A extension, as in the interpreter (exec_atomic()): the host's atomic operations. */
		const uint64_t mem = 0;	/* (the operations take the host address, from L_HOST) */
		uint32_t f5 = i >> 27;
		int wide = f3 == 3;
		static const uint8_t rmw[16] = {
			[0x00] = W_ATOMIC_ADD, [0x01] = W_ATOMIC_XCHG, [0x04] = W_ATOMIC_XOR,
			[0x08] = W_ATOMIC_OR, [0x0c] = W_ATOMIC_AND,
		};

		effective_address(g, rs1, 0);
		check_access_as(g, insn, not_run, wide ? 8 : 4,
				f5 == 0x02 ? RV_PROT_READ : RV_PROT_READ | RV_PROT_WRITE, 1, 0);

		if (f5 == 0x02) {
			/* LR: the value, and a note of where it was read and what it was. */
			w_local(b, W_LOCAL_GET, L_HOST);
			w_atomic(b, W_ATOMIC_LOAD, wide, mem);
			if (!wide)
				w_op(b, W_I64_EXTEND_I32_S);
			w_local(b, W_LOCAL_SET, L_TMP);
			w_local(b, W_LOCAL_GET, L_CPU);
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_mem(b, W_I64_STORE, 3, offsetof(struct rv_cpu, lr_addr));
			w_local(b, W_LOCAL_GET, L_CPU);
			w_local(b, W_LOCAL_GET, L_TMP);
			w_mem(b, W_I64_STORE, 3, offsetof(struct rv_cpu, lr_value));
			w_local(b, W_LOCAL_GET, L_CPU);
			w_i32(b, 1);
			w_mem(b, W_I32_STORE, 2, offsetof(struct rv_cpu, lr_valid));
			w_local(b, W_LOCAL_GET, L_TMP);
			set_reg(g, rd);
		} else if (f5 == 0x03) {
			/*
			 * SC: if the note is of this address, store unless the value
			 * has changed since, in one step. rd is 0 if it was stored.
			 */
			w_local(b, W_LOCAL_GET, L_CPU);
			w_mem(b, W_I32_LOAD, 2, offsetof(struct rv_cpu, lr_valid));
			w_local(b, W_LOCAL_GET, L_CPU);
			w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, lr_addr));
			w_local(b, W_LOCAL_GET, L_ADDR);
			w_op(b, W_I64_EQ);
			w_op(b, W_I32_AND);
			w_local(b, W_LOCAL_GET, L_CPU);
			w_i32(b, 0);
			w_mem(b, W_I32_STORE, 2, offsetof(struct rv_cpu, lr_valid));
			w_byte(b, W_IF);
			w_byte(b, W_TYPE_I64);
			/* What is there now (and replaced, if it is the value noted)... */
			w_local(b, W_LOCAL_GET, L_HOST);
			w_local(b, W_LOCAL_GET, L_CPU);
			w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, lr_value));
			if (!wide)
				w_op(b, W_I32_WRAP_I64);
			get_reg(g, rs2);
			if (!wide)
				w_op(b, W_I32_WRAP_I64);
			w_atomic(b, W_ATOMIC_CMPXCHG, wide, mem);
			if (!wide)
				w_op(b, W_I64_EXTEND_I32_S);
			/* ... is not the value noted: 1. */
			w_local(b, W_LOCAL_GET, L_CPU);
			w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, lr_value));
			w_op(b, W_I64_NE);
			w_op(b, W_I64_EXTEND_I32_U);
			w_byte(b, W_ELSE);
			w_i64(b, 1);
			w_op(b, W_END);
			set_reg(g, rd);
		} else {
			/* AMOSWAP, AMOADD, AMOXOR, AMOOR, AMOAND: rd is what was there. */
			w_local(b, W_LOCAL_GET, L_HOST);
			get_reg(g, rs2);
			if (!wide)
				w_op(b, W_I32_WRAP_I64);
			w_atomic(b, rmw[f5 & 15], wide, mem);
			if (!wide)
				w_op(b, W_I64_EXTEND_I32_S);
			set_reg(g, rd);
		}
		break;
	}

	case 0x07: /* LOAD-FP: FLW (the value is NaN-boxed) and FLD */
		effective_address(g, rs1, IMM_I(i));
		check_access(g, insn, not_run, f3 == 2 ? 4 : 8, RV_PROT_READ, rs1);
		freg_put_begin(g);
		w_local(b, W_LOCAL_GET, L_HOST);
		if (f3 == 2) {
			w_mem(b, W_I64_LOAD32_U, 0, 0);
			w_i64(b, 0xffffffff00000000ULL);
			w_op(b, W_I64_OR);
		} else {
			w_mem(b, W_I64_LOAD, 0, 0);
		}
		freg_put_end(g, rd);
		break;

	case 0x27: /* STORE-FP: FSW and FSD */
		effective_address(g, rs1, IMM_S(i));
		check_access(g, insn, not_run, f3 == 2 ? 4 : 8, RV_PROT_WRITE, rs1);
		w_local(b, W_LOCAL_GET, L_HOST);
		freg_get(g, rs2);
		w_mem(b, f3 == 2 ? W_I64_STORE32 : W_I64_STORE, 0, 0);
		break;

	case 0x43: case 0x47: case 0x4b: case 0x4f: /* FMADD, FMSUB, FNMSUB, FNMADD */
		if (!(g->cpu->jit->off & OFF_FPHOST) && gen_fp_fused(g, insn, not_run))
			break;
		gen_fp_call(g, insn, not_run, 0);
		break;

	case 0x53: /* the rest of F and D: by the host where that is exact, else as below */
		if (!(g->cpu->jit->off & OFF_FPHOST) && gen_fp_host(g, insn, not_run))
			break;
		/* fall through */
	case 0x73: /* Zicsr */
		if (OPCODE(i) == 0x73 && gen_csr(g, insn))
			break;
		gen_fp_call(g, insn, not_run, 0);
		if (OPCODE(i) == 0x73 || rv_fp_int_dest(i)) {
			w_local(b, W_LOCAL_GET, L_TMP);
			set_reg(g, rd);
		}
		break;
	}
}

/* ------------------------------------------------------------ region layout */

/* The edges of the region's blocks: static successors, calls' return addresses, indirect jumps seen (see struct layout). */
static void layout_edges(struct rv_cpu *cpu, const struct jit_region *r, struct layout *lay)
{
	memset(lay->succ, 0, sizeof(lay->succ));
	for (int k = 0; k < r->nblocks; k++) {
		const struct jit_block *blk = &r->blocks[k];
		const struct jit_insn *last = &r->insns[blk->first + blk->count - 1];
		enum kind kind = classify(last->raw);
		int t;

		switch (kind) {
		case K_BRANCH:
			if ((t = find_block(r, last->pc + (uint64_t)imm_b(last->raw))) >= 0)
				lay->succ[k][t / 64] |= 1ULL << (t % 64);
			if ((t = find_block(r, blk->next_pc)) >= 0)
				lay->succ[k][t / 64] |= 1ULL << (t % 64);
			break;
		case K_JAL:
			if ((t = find_block(r, last->pc + (uint64_t)imm_j(last->raw))) >= 0)
				lay->succ[k][t / 64] |= 1ULL << (t % 64);
			/* A call comes back to the next instruction, by way of the callee. */
			if (RD(last->raw) && (t = find_block(r, blk->next_pc)) >= 0)
				lay->succ[k][t / 64] |= 1ULL << (t % 64);
			break;
		case K_JALR: {
			uint64_t targets[JALR_TARGETS];
			int n = jalr_targets(cpu->jit, last->pc, targets);

			for (int i = 0; i < n; i++)
				if ((t = find_block(r, targets[i])) >= 0)
					lay->succ[k][t / 64] |= 1ULL << (t % 64);
			break;
		}
		default:
			if ((t = find_block(r, blk->next_pc)) >= 0)
				lay->succ[k][t / 64] |= 1ULL << (t % 64);
		}
	}
}

/*
 * Tarjan's strongly connected components of the blocks in `in`, edges into
 * `cut` left out. Emitted in reverse topological order: a component after
 * every component it has an edge to.
 */
struct tarjan {
	const struct layout *lay;
	const uint64_t *in;
	int cut, n;
	int index[JIT_MAX_BLOCKS], low[JIT_MAX_BLOCKS], stack[JIT_MAX_BLOCKS];
	uint8_t on[JIT_MAX_BLOCKS];
	int next, sp;
	int comp[JIT_MAX_BLOCKS];	/* the component of each block, numbered in emission order */
	int ncomp;
};

static int in_set(const uint64_t *set, int b)
{
	return (set[b / 64] >> (b % 64)) & 1;
}

static void tarjan_visit(struct tarjan *t, int v)
{
	t->index[v] = t->low[v] = t->next++;
	t->stack[t->sp++] = v;
	t->on[v] = 1;
	for (int w = 0; w < t->n; w++) {
		if (!in_set(t->lay->succ[v], w) || !in_set(t->in, w) || w == t->cut)
			continue;
		if (t->index[w] < 0) {
			tarjan_visit(t, w);
			if (t->low[w] < t->low[v])
				t->low[v] = t->low[w];
		} else if (t->on[w] && t->index[w] < t->low[v]) {
			t->low[v] = t->index[w];
		}
	}
	if (t->low[v] == t->index[v]) {
		int w;

		do {
			w = t->stack[--t->sp];
			t->on[w] = 0;
			t->comp[w] = t->ncomp;
		} while (w != v);
		t->ncomp++;
	}
}

/*
 * Lay out the blocks in `in` (edges into `cut` left out) as the items of
 * scope `s`: in topological order, each component of more than one block
 * (or one that jumps to itself) as a nested scope. Returns -1 if the tables
 * are full.
 */
static int layout_scope(struct layout *lay, int n, const uint64_t *in, int cut, int s)
{
	struct tarjan *t = malloc(sizeof(*t));
	int order[JIT_MAX_BLOCKS], norder = 0, first_item = lay->nitems, ret = 0;

	if (!t)
		return -1;
	t->lay = lay;
	t->in = in;
	t->cut = cut;
	t->n = n;
	t->next = t->sp = t->ncomp = 0;
	for (int v = 0; v < n; v++) {
		t->index[v] = -1;
		t->on[v] = 0;
	}
	for (int v = 0; v < n; v++)
		if (in_set(in, v) && t->index[v] < 0)
			tarjan_visit(t, v);

	/* Components in topological order (Tarjan's is the reverse), the header of this scope first of all. */
	for (int c = t->ncomp - 1; c >= 0; c--)
		order[norder++] = c;
	lay->scopes[s].first = first_item;
	lay->scopes[s].nitems = 0;
	lay->scopes[s].lookup = -1;
	/* Room: an item for each component, which is at most one per block. */
	if (lay->nitems + norder > (int)(sizeof(lay->items) / sizeof(lay->items[0]))) {
		free(t);
		return -1;
	}
	lay->nitems += norder;
	{
		int at = first_item;

		/* The header is a source now (its edges in are cut), so it may go first. */
		if (cut >= 0) {
			for (int i = 0; i < norder; i++) {
				if (order[i] == t->comp[cut]) {
					int c = order[i];

					memmove(order + 1, order, (size_t)i * sizeof(order[0]));
					order[0] = c;
					break;
				}
			}
		}
		for (int i = 0; i < norder && ret == 0; i++) {
			int c = order[i], member = -1, count = 0, self = 0;

			for (int v = 0; v < n; v++) {
				if (in_set(in, v) && t->comp[v] == c) {
					member = v;
					count++;
					self |= in_set(lay->succ[v], v) && v != cut;
				}
			}
			if (count == 1 && !self) {
				lay->items[at] = member;
				lay->block_scope[member] = s;
				lay->block_item[member] = at - first_item;
				at++;
				continue;
			}
			/* A loop: its header is the block most entered from outside the component. */
			{
				uint64_t set[(JIT_MAX_BLOCKS + 63) / 64] = { 0 };
				int header = -1, best = -1, ns;

				for (int v = 0; v < n; v++) {
					int entries = 0;

					if (!in_set(in, v) || t->comp[v] != c)
						continue;
					set[v / 64] |= 1ULL << (v % 64);
					for (int u = 0; u < n; u++)
						if (in_set(lay->succ[u], v) && (!in_set(in, u) || t->comp[u] != c || u == cut))
							entries++;
					if (entries > best) {
						best = entries;
						header = v;
					}
				}
				if (lay->nscopes == SCOPE_MAX) {
					ret = -1;
					break;
				}
				ns = lay->nscopes++;
				lay->scopes[ns].parent = s;
				lay->scopes[ns].parent_item = at - first_item;
				lay->scopes[ns].header = header;
				lay->items[at] = ITEM_SCOPE(ns);
				at++;
				ret = layout_scope(lay, n, set, header, ns);
			}
		}
		lay->scopes[s].nitems = at - first_item;
	}
	free(t);
	return ret;
}

/*
 * Which scopes get a lookup (gen_lookup()), placed after the header: the top,
 * if the region has an indirect jump at all (every jump can fall back to
 * it), and a loop of its own if it has a jump with many targets (an
 * interpreter's dispatch), so that those go on inside the loop without
 * passing through the top. A jump with few targets compares with each of
 * them where it is (gen_insn(), JALR) and needs no lookup of its own; one
 * lookup is a few KiB of code, so they are not handed out freely.
 */
static void layout_lookups(struct rv_cpu *cpu, struct layout *lay, const struct jit_region *r)
{
	for (int s = 0; s < lay->nscopes; s++) {
		struct scope *sc = &lay->scopes[s];
		int wanted = 0, at;

		for (int i = 0; i < sc->nitems && !wanted; i++) {
			int v = lay->items[sc->first + i];
			const struct jit_insn *last;

			if (v < 0)
				continue;
			last = &r->insns[r->blocks[v].first + r->blocks[v].count - 1];
			if (OPCODE(last->raw) != 0x67)
				continue;
			if (s == 0) {
				wanted = 1;
			} else {
				uint64_t targets[JALR_TARGETS];

				wanted = jalr_targets(cpu->jit, last->pc, targets) > JALR_INLINE;
			}
		}
		if (s == 0 && !wanted) {
			/* Any indirect jump anywhere: the top has the lookup they all can fall back to. */
			for (int k = 0; k < r->nblocks && !wanted; k++)
				wanted = OPCODE(r->insns[r->blocks[k].first + r->blocks[k].count - 1].raw) == 0x67;
		}
		if (!wanted)
			continue;
		/* After the header (item 0) of a loop; first of all at the top. Everything after (other scopes' items too: one array) moves up one. */
		at = sc->header >= 0 ? 1 : 0;
		memmove(&lay->items[sc->first + at + 1], &lay->items[sc->first + at], (size_t)(lay->nitems - (sc->first + at)) * sizeof(lay->items[0]));
		lay->items[sc->first + at] = ITEM_LOOKUP;
		sc->nitems++;
		sc->lookup = at;
		for (int i = at + 1; i < sc->nitems; i++) {
			int v = lay->items[sc->first + i];

			if (v >= 0)
				lay->block_item[v] = i;
			else if (v <= -2)
				lay->scopes[SCOPE_OF_ITEM(v)].parent_item = i;
		}
		/* Scopes after this one have their items further along as well. */
		for (int o = 0; o < lay->nscopes; o++)
			if (lay->scopes[o].first > sc->first)
				lay->scopes[o].first++;
		lay->nitems++;
	}
}

/* The layout of region r (see struct layout). 0, or -1 if it does not fit, in which case everything is one flat scope. */
static int layout_region(struct rv_cpu *cpu, const struct jit_region *r, struct layout *lay)
{
	uint64_t all[(JIT_MAX_BLOCKS + 63) / 64] = { 0 };
	int n = r->nblocks;

	layout_edges(cpu, r, lay);
	for (int v = 0; v < n; v++)
		all[v / 64] |= 1ULL << (v % 64);
	lay->nscopes = 1;
	lay->nitems = 0;
	lay->scopes[0].parent = -1;
	lay->scopes[0].parent_item = -1;
	lay->scopes[0].header = -1;
	if ((cpu->jit->off & OFF_STRUCT) || layout_scope(lay, n, all, -1, 0) < 0 ||
	    lay->nitems + lay->nscopes > (int)(sizeof(lay->items) / sizeof(lay->items[0]))) {
		/* Flat: every block an item of the top scope, every jump through its dispatcher. */
		lay->nscopes = 1;
		lay->scopes[0].first = 0;
		lay->scopes[0].nitems = n;
		lay->scopes[0].lookup = -1;
		lay->nitems = n;
		for (int v = 0; v < n; v++) {
			lay->items[v] = v;
			lay->block_scope[v] = 0;
			lay->block_item[v] = v;
		}
		memset(lay->succ, 0, sizeof(lay->succ));
	}
	layout_lookups(cpu, lay, r);
	return 0;
}

/*
 * Number the blocks in the order of the layout (a scope's items in order, a
 * nested scope's blocks where its item is): the blocks array is permuted so
 * that a block's index is its number, and every scope's blocks are then
 * lo..hi, which is what its dispatcher's br_table spans. Done once, before
 * anything else (the map, the table of targets, the copy kept) uses the
 * indices.
 */
static void renumber_visit(struct layout *lay, int s, int *number, int *next)
{
	struct scope *sc = &lay->scopes[s];

	sc->lo = *next;
	for (int k = 0; k < sc->nitems; k++) {
		int v = lay->items[sc->first + k];

		if (v >= 0)
			number[v] = (*next)++;
		else if (v <= -2)
			renumber_visit(lay, SCOPE_OF_ITEM(v), number, next);
	}
	sc->hi = *next - 1;
}

static void layout_renumber(struct jit_region *r, struct layout *lay)
{
	static struct jit_block blocks[JIT_MAX_BLOCKS];	/* (under the scratch lock, like r itself) */
	int number[JIT_MAX_BLOCKS], scope_of[JIT_MAX_BLOCKS], item_of[JIT_MAX_BLOCKS], next = 0;

	renumber_visit(lay, 0, number, &next);
	for (int k = 0; k < r->nblocks; k++) {
		blocks[number[k]] = r->blocks[k];
		scope_of[number[k]] = lay->block_scope[k];
		item_of[number[k]] = lay->block_item[k];
	}
	memcpy(r->blocks, blocks, (size_t)r->nblocks * sizeof(blocks[0]));
	memcpy(lay->block_scope, scope_of, (size_t)r->nblocks * sizeof(scope_of[0]));
	memcpy(lay->block_item, item_of, (size_t)r->nblocks * sizeof(item_of[0]));
	for (int i = 0; i < lay->nitems; i++)
		if (lay->items[i] >= 0)
			lay->items[i] = number[lay->items[i]];
	for (int sc = 0; sc < lay->nscopes; sc++)
		if (lay->scopes[sc].header >= 0)
			lay->scopes[sc].header = number[lay->scopes[sc].header];
}

/* -------------------------------------------------------------- the body */

/*
 * The lookup of a scope, for its indirect jumps (JALR comes here with the
 * target in $pc): the block whose address it is, found by hashing the address
 * (as target_slot() does) into a br_table of the region's table of targets
 * (target_table()), each entry a comparison; the region goes on there, or it
 * leaves for the target. The table is code, not data in memory: so a region
 * has no address of the thread's in it, and a region that several threads
 * make the same way is the same bytes, which the browser's engine compiles
 * and keeps once for all of them (V8's native module cache).
 */
static void gen_lookup(struct gen *g)
{
	struct wbuf *b = g->b;
	struct rv_jit *jit = g->cpu->jit;
	uint32_t entries = 1u << jit->target_bits;

	w_byte(b, W_BLOCK);		/* not found: out of this */
	w_byte(b, W_TYPE_VOID);
	for (uint32_t k = 0; k < entries; k++) {
		w_byte(b, W_BLOCK);
		w_byte(b, W_TYPE_VOID);
	}
	w_local(b, W_LOCAL_GET, L_PC);
	w_i64(b, 1);
	w_op(b, W_I64_SHR_U);
	w_i64(b, jit->target_multiplier);
	w_op(b, W_I64_MUL);
	w_i64(b, 64 - jit->target_bits);
	w_op(b, W_I64_SHR_U);
	w_op(b, W_I32_WRAP_I64);
	w_byte(b, W_BR_TABLE);
	w_uleb(b, entries);
	for (uint32_t k = 0; k < entries; k++)
		w_uleb(b, k);
	w_uleb(b, entries - 1);
	for (uint32_t k = 0; k < entries; k++) {
		const struct jit_target *e = &jit->targets_now[k];
		uint32_t inside = entries - 1 - k;	/* the entries' blocks still open */

		w_op(b, W_END);
		if (e->pc != TARGET_NONE) {
			w_local(b, W_LOCAL_GET, L_PC);
			w_i64(b, e->pc);
			w_op(b, W_I64_EQ);
			w_byte(b, W_IF);
			w_byte(b, W_TYPE_VOID);
			branch_to_block(g, (int)e->index, 1 + inside + 1);
			w_op(b, W_END);
		}
		if (inside)
			w_br(b, W_BR, inside);
	}
	w_op(b, W_END);			/* not found */
	leave_for_target(g, 0);
}

/* Whether gen_insn() checks a memory access for the instruction (calls check_access_as()). */
static int insn_accesses(uint32_t i)
{
	if (rv_zb_kind(i))
		return 0;
	switch (OPCODE(i)) {
	case 0x03: case 0x23: case 0x2f: case 0x07: case 0x27:
		return 1;
	}
	return 0;
}

/* An integer register the instruction may write (0: none), for gen_groups(): erring towards yes costs only a group. */
static uint32_t insn_writes(uint32_t i)
{
	switch (OPCODE(i)) {
	case 0x23: case 0x27: case 0x63: case 0x0f: case 0x07:
	case 0x43: case 0x47: case 0x4b: case 0x4f:
		return 0;
	}
	return RD(i);
}

/*
 * OFF_HOIST: the groups of block k's accesses (see check_access_as()). Loads
 * and stores (not atomics) by the same base register, while it is not
 * written (other than by a small step: addi r, r, imm), in at most
 * CACHE_SLOTS groups at a time; a group of one is none. Which accesses are in
 * a group only decides where the test is cheap: each access of a group tests
 * that it is in the group's page, so none is ever wrong.
 */
static void gen_groups(struct gen *g, const struct jit_block *blk)
{
	int reg[CACHE_SLOTS], first[CACHE_SLOTS], members[CACHE_SLOTS];
	uint32_t next = 0;

	for (int s = 0; s < CACHE_SLOTS; s++)
		reg[s] = -1;
	for (uint32_t j = 0; j < blk->count; j++) {
		uint32_t i = g->r->insns[blk->first + j].raw, w;
		int s;

		g->role[j] = ACC_PLAIN;
		if (insn_accesses(i) && OPCODE(i) != 0x2f) {
			unsigned prot = OPCODE(i) == 0x23 || OPCODE(i) == 0x27 ? RV_PROT_WRITE : RV_PROT_READ;

			for (s = 0; s < CACHE_SLOTS && reg[s] != (int)RS1(i); s++)
				;
			if (s < CACHE_SLOTS) {
				g->role[j] = ACC_MEMBER;
				g->slot[j] = (uint8_t)s;
				g->uprot[first[s]] |= (uint8_t)prot;
				members[s]++;
			} else {
				for (s = 0; s < CACHE_SLOTS && reg[s] >= 0; s++)
					;
				if (s == CACHE_SLOTS)
					s = (int)(next++ % CACHE_SLOTS);
				if (reg[s] >= 0 && !members[s])
					g->role[first[s]] = ACC_PLAIN;
				reg[s] = (int)RS1(i);
				first[s] = (int)j;
				members[s] = 0;
				g->role[j] = ACC_FIRST;
				g->slot[j] = (uint8_t)s;
				g->uprot[j] = (uint8_t)prot;
			}
		}
		w = insn_writes(i);
		/* addi r, r, small (a stack frame, a pointer stepping through an array): r most likely stays in the page. */
		if (OPCODE(i) == 0x13 && F3(i) == 0 && RS1(i) == w && IMM_I(i) > -2048 && IMM_I(i) < 2048)
			w = 0;
		for (s = 0; w && s < CACHE_SLOTS; s++) {
			if (reg[s] == (int)w) {
				if (!members[s])
					g->role[first[s]] = ACC_PLAIN;
				reg[s] = -1;
			}
		}
	}
	for (int s = 0; s < CACHE_SLOTS; s++)
		if (reg[s] >= 0 && !members[s])
			g->role[first[s]] = ACC_PLAIN;
}

/* Leave if a signal is pending or the budget is spent. `pc`: what $pc is to be then, or 0 if it is right already. */
static void gen_check(struct gen *g, uint64_t pc, uint32_t extra)
{
	struct wbuf *b = g->b;

	w_local(b, W_LOCAL_GET, L_BUDGET);
	w_i32(b, 0);
	w_op(b, W_I32_LT_S);
	w_local(b, W_LOCAL_GET, L_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, sig_pending));
	w_i64(b, 0);
	w_op(b, W_I64_NE);
	w_op(b, W_I32_OR);
	hint(g, HS_POLL, 0);
	if (!pc) {
		w_br(b, W_BR_IF, exit_depth(g, extra));
		return;
	}
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_i64(b, pc);
	w_local(b, W_LOCAL_SET, L_PC);
	w_br(b, W_BR, exit_depth(g, extra + 1));
	w_op(b, W_END);
}

/* br_table on $next: to the item of scope s that has that block (or lookup). */
static void gen_dispatch(struct gen *g, int s, uint32_t extra)
{
	const struct layout *lay = g->lay;
	const struct scope *sc = &lay->scopes[s];
	struct wbuf *b = g->b;
	/* Out of the range: $next names this scope's lookup (nblocks + scope: see JALR in gen_insn()), or nothing is asked for. */
	int fallback = sc->lookup >= 0 ? sc->lookup : 0;

	w_local(b, W_LOCAL_GET, L_NEXT);
	if (sc->lo) {
		w_i32(b, sc->lo);
		w_op(b, W_I32_SUB);
	}
	w_byte(b, W_BR_TABLE);
	w_uleb(b, (uint64_t)(sc->hi - sc->lo + 1));
	for (int v = sc->lo; v <= sc->hi; v++) {
		int item = item_at(lay, s, lay->block_scope[v], lay->block_item[v]);

		w_uleb(b, (uint64_t)(item < 0 ? fallback : item) + extra);
	}
	w_uleb(b, (uint64_t)fallback + extra);
}

static void gen_scope(struct gen *g, int s);

static void gen_block(struct gen *g, int k)
{
	const struct jit_region *r = g->r;
	const struct jit_block *blk = &r->blocks[k];
	const struct jit_insn *last = &r->insns[blk->first + blk->count - 1];
	struct wbuf *b = g->b;

	/*
	 * The self-check (checked_call()) runs regions one block at a time, by
	 * giving them no budget: so with it every block looks at the budget,
	 * where otherwise only the loops' tops do.
	 */
	if (g->cpu->jit->check)
		gen_check(g, blk->pc, 0);
	w_local(b, W_LOCAL_GET, L_BUDGET);
	w_i32(b, (int32_t)blk->count);
	w_op(b, W_I32_SUB);
	w_local(b, W_LOCAL_SET, L_BUDGET);
	if (g->hoist)
		gen_groups(g, blk);
	for (uint32_t j = 0; j < blk->count; j++) {
		g->j = j;
		gen_insn(g, &r->insns[blk->first + j], blk->count - j);
	}
	if (classify(last->raw) == K_PLAIN)
		transition(g, blk->next_pc, 0);
}

/* The items of scope s inside its loop (the caller opened it): the ladder, the dispatcher, the items. */
static void gen_scope(struct gen *g, int s)
{
	const struct layout *lay = g->lay;
	const struct scope *sc = &lay->scopes[s];
	struct wbuf *b = g->b;

	for (int k = 0; k < sc->nitems; k++) {
		w_byte(b, W_BLOCK);
		w_byte(b, W_TYPE_VOID);
	}
	g->scope = s;
	g->item = -1;
	if (sc->header < 0) {
		/* The top: the check, then wherever $next says. */
		gen_check(g, 0, 0);
		gen_dispatch(g, s, 0);
	} else {
		/* A loop: entered at its header (a back edge, mostly), or elsewhere through $next. */
		w_local(b, W_LOCAL_GET, L_NEXT);
		w_i32(b, sc->header);
		w_op(b, W_I32_NE);
		w_byte(b, W_IF);
		w_byte(b, W_TYPE_VOID);
		gen_dispatch(g, s, 1);
		w_op(b, W_END);
		gen_check(g, g->r->blocks[sc->header].pc, 0);
	}
	for (int k = 0; k < sc->nitems; k++) {
		int v = lay->items[sc->first + k];

		w_op(b, W_END);
		g->scope = s;
		g->item = k;
		if (v >= 0) {
			gen_block(g, v);
		} else if (v == ITEM_LOOKUP) {
			gen_lookup(g);
		} else {
			w_byte(b, W_LOOP);
			w_byte(b, W_TYPE_VOID);
			gen_scope(g, SCOPE_OF_ITEM(v));
			w_op(b, W_END);
			g->scope = s;
			g->item = k;
		}
	}
}

/* The body of the region function, without the register loads and stores around it. */
static void gen_body(struct gen *g)
{
	struct wbuf *b = g->b;

	w_byte(b, W_BLOCK);		/* leaving the region branches to the end of this */
	w_byte(b, W_TYPE_VOID);
	w_byte(b, W_LOOP);
	w_byte(b, W_TYPE_VOID);
	gen_scope(g, 0);
	w_op(b, W_END);	/* loop */
	w_op(b, W_END);	/* block */
}

/*
 * The module's own function for an access beyond the page table, of the
 * type of a helper with three arguments: (cpu, addr, size | prot << 8 |
 * aligned << 16) -> the host address in a reservation, or 0. It is
 * rv_reserve_xlat() (mm.c) for the case that matters, here, without the
 * call into the emulator: if the thread's reserve_seen slot for the page is
 * the page's and its count is the process's, the entry at that place in the
 * table is looked at as for any page (if not, the place looked at is
 * npages, whose entry allows nothing). Otherwise rv_jit_reserve() is asked.
 * Each region's module has one (a call within the module costs next to
 * nothing; the same code at every access made regions a third larger, and
 * the host's compiling of them as much slower). Everything it looks at is
 * found through A_CPU, so the function is the same in every thread and
 * every process.
 */
static void gen_reserve_fn(struct rv_cpu *cpu, struct wbuf *b)
{
	enum { A_CPU, A_ADDR, A_HOW, T_AT };
	uint64_t seen = offsetof(struct rv_cpu, reserve_seen);	/* (from A_CPU: the thread's) */
	enum { T_MAP = T_AT + 1 };	/* cpu->map */

	(void)cpu;
	w_uleb(b, 1);	/* one group of locals: two i64 */
	w_uleb(b, 2);
	w_byte(b, W_TYPE_I64);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, map));
	w_local(b, W_LOCAL_SET, T_MAP);
	/*
	 * An address in the table: its own entry, looked at as below (an access
	 * of a group in another page than the group's, see check_access_as(); for
	 * any other access it failed the same test already, and fails it again).
	 */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_op(b, W_I64_LT_U);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_local(b, W_LOCAL_SET, T_AT);
	w_byte(b, W_ELSE);
	/* T_AT = the slot's offset in reserve_seen */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_i64(b, sizeof(cpu->reserve_seen) / sizeof(cpu->reserve_seen[0]) - 1);
	w_op(b, W_I64_AND);
	w_i64(b, sizeof(cpu->reserve_seen[0]));
	w_op(b, W_I64_MUL);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_op(b, W_I64_ADD);
	w_local(b, W_LOCAL_SET, T_AT);
	/* T_AT = select(slot.flat, npages, slot.page == page && slot.gen == reserve_gen) */
	w_local(b, W_LOCAL_GET, T_AT);
	w_mem(b, W_I64_LOAD, 3, seen + offsetof(struct rv_reserve_seen, flat));
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_local(b, W_LOCAL_GET, T_AT);
	w_mem(b, W_I64_LOAD, 3, seen + offsetof(struct rv_reserve_seen, page));
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_op(b, W_I64_EQ);
	w_local(b, W_LOCAL_GET, T_AT);
	w_mem(b, W_I64_LOAD, 3, seen + offsetof(struct rv_reserve_seen, gen));
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, proc));
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_proc, reserve_gen));
	w_op(b, W_I64_EQ);
	w_op(b, W_I32_AND);
	w_op(b, W_SELECT);
	w_local(b, W_LOCAL_SET, T_AT);
	w_op(b, W_END);
	/* (map[T_AT] & prot) == prot */
	w_local(b, W_LOCAL_GET, T_AT);
	w_i64(b, 3);
	w_op(b, W_I64_SHL);
	w_local(b, W_LOCAL_GET, T_MAP);
	w_op(b, W_I64_ADD);
	w_mem(b, W_I64_LOAD, 3, 0);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 8);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 8);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_EQ);
	/* && (addr & 4095) + size <= 4096 */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SIZE - 1);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_ADD);
	w_i64(b, RV_PAGE_SIZE);
	w_op(b, W_I64_LE_U);
	w_op(b, W_I32_AND);
	/* && (!aligned || !(addr & (size - 1))) */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_i64(b, 1);
	w_op(b, W_I64_SUB);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_EQZ);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 1 << 16);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_EQZ);
	w_op(b, W_I32_OR);
	w_op(b, W_I32_AND);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	/* (map[T_AT] & ~flags) + (T_AT << 12) + (addr & 4095) */
	w_local(b, W_LOCAL_GET, T_AT);
	w_i64(b, 3);
	w_op(b, W_I64_SHL);
	w_local(b, W_LOCAL_GET, T_MAP);
	w_op(b, W_I64_ADD);
	w_mem(b, W_I64_LOAD, 3, 0);
	w_i64(b, ~(uint64_t)RV_PAGE_FLAGS);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_GET, T_AT);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHL);
	w_op(b, W_I64_ADD);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SIZE - 1);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_ADD);
	w_op(b, W_RETURN);
	w_op(b, W_END);
	/* Not allowed in the table: the interpreter's (it raises the fault, or marks the code changed, or ...). */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_op(b, W_I64_LT_U);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_i64(b, 0);
	w_op(b, W_RETURN);
	w_op(b, W_END);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_byte(b, W_CALL);
	w_uleb(b, H_RESERVE);
	w_op(b, W_END);
}

/*
 * The module's function for a memory access that missed its translation
 * cache (check_access()): (cpu, addr, size | prot << 8) -> the host address,
 * or 0 if the access is not to be made as it stands (the region leaves it to
 * the interpreter). The check is rv_ptr()'s: the page's entry allows prot
 * and the access is within the page; else the page may be in a reservation
 * (F_RESERVE). It leaves in cpu->xlat what the caller puts into the
 * register's cache: the page, for reading and for writing as the entry
 * allows (CACHE_NONE where it does not), and the distance to the host
 * address. Everything is found through its first argument, so the function
 * is the same in every thread and process.
 */
static void gen_xlat_fn(struct wbuf *b)
{
	enum { A_CPU, A_ADDR, A_HOW, T_E, T_PAGE };

	w_uleb(b, 1);	/* one group of locals: two i64 */
	w_uleb(b, 2);
	w_byte(b, W_TYPE_I64);
	/* T_PAGE = min(addr >> 12, npages): the entry at npages allows nothing */
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SHIFT);
	w_op(b, W_I64_SHR_U);
	w_local(b, W_LOCAL_TEE, T_PAGE);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_local(b, W_LOCAL_GET, T_PAGE);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_op(b, W_I64_LT_U);
	w_op(b, W_SELECT);
	w_local(b, W_LOCAL_SET, T_PAGE);
	/* T_E = map[T_PAGE] */
	w_local(b, W_LOCAL_GET, T_PAGE);
	w_i64(b, 3);
	w_op(b, W_I64_SHL);
	w_local(b, W_LOCAL_GET, A_CPU);
	w_mem(b, W_I64_LOAD, 3, offsetof(struct rv_cpu, map));
	w_op(b, W_I64_ADD);
	w_mem(b, W_I64_LOAD, 3, 0);
	w_local(b, W_LOCAL_SET, T_E);
	/* (e & prot) != prot || (addr & 4095) + size > 4096: not as it stands */
	w_local(b, W_LOCAL_GET, T_E);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 8);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 8);
	w_op(b, W_I64_SHR_U);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_NE);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_i64(b, RV_PAGE_SIZE - 1);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 0xff);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_ADD);
	w_i64(b, RV_PAGE_SIZE);
	w_op(b, W_I64_GT_U);
	w_op(b, W_I32_OR);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	/* A reservation? The host address, or 0; as an entry: the distance, allowing what was asked. */
	w_local(b, W_LOCAL_GET, A_CPU);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_byte(b, W_CALL);
	w_uleb(b, F_RESERVE);
	w_local(b, W_LOCAL_TEE, T_E);
	w_op(b, W_I64_EQZ);
	w_byte(b, W_IF);
	w_byte(b, W_TYPE_VOID);
	w_i64(b, 0);
	w_op(b, W_RETURN);
	w_op(b, W_END);
	w_local(b, W_LOCAL_GET, T_E);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_op(b, W_I64_SUB);
	w_local(b, W_LOCAL_GET, A_HOW);
	w_i64(b, 8);
	w_op(b, W_I64_SHR_U);
	w_i64(b, RV_PROT_MASK);
	w_op(b, W_I64_AND);
	w_op(b, W_I64_OR);
	w_local(b, W_LOCAL_SET, T_E);
	w_op(b, W_END);
	/* cpu->xlat: the page if readable, the page if writable, the distance */
	for (int w = 0; w < 2; w++) {
		w_local(b, W_LOCAL_GET, A_CPU);
		w_local(b, W_LOCAL_GET, A_ADDR);
		w_i64(b, ~(uint64_t)(RV_PAGE_SIZE - 1));
		w_op(b, W_I64_AND);
		w_i64(b, CACHE_NONE);
		w_local(b, W_LOCAL_GET, T_E);
		w_op(b, W_I32_WRAP_I64);
		w_i32(b, w ? RV_PROT_WRITE : RV_PROT_READ);
		w_op(b, W_I32_AND);
		w_op(b, W_SELECT);
		w_mem(b, W_I64_STORE, 3, offsetof(struct rv_cpu, xlat) + 8 * w);
	}
	w_local(b, W_LOCAL_GET, A_CPU);
	w_local(b, W_LOCAL_GET, T_E);
	w_i64(b, ~(uint64_t)RV_PAGE_FLAGS);
	w_op(b, W_I64_AND);
	w_local(b, W_LOCAL_TEE, T_E);
	w_mem(b, W_I64_STORE, 3, offsetof(struct rv_cpu, xlat) + 16);
	/* The host address. */
	w_local(b, W_LOCAL_GET, T_E);
	w_local(b, W_LOCAL_GET, A_ADDR);
	w_op(b, W_I64_ADD);
	w_op(b, W_END);
}

/* Wrap the body into a complete module in jit->module. */
/* The start of every module of regions: the header, the types and the imports. */
static void gen_module_head(struct wbuf *m)
{
	static uint8_t tmp_data[1024];
	struct wbuf tmp = { tmp_data, 0, sizeof(tmp_data), 0 };

	w_bytes(m, "\0asm\1\0\0\0", 8);

	/* Types: 0 = region (cpu, entry, budget), 1 = helper (a, b) -> result, 2 = helper (cpu, insn, a) -> result. */
	tmp.len = 0;
	w_uleb(&tmp, 3);
	w_byte(&tmp, W_TYPE_FUNC);
	w_uleb(&tmp, 3); w_byte(&tmp, W_TYPE_I64); w_byte(&tmp, W_TYPE_I32); w_byte(&tmp, W_TYPE_I32);
	w_uleb(&tmp, 0);
	w_byte(&tmp, W_TYPE_FUNC);
	w_uleb(&tmp, 2); w_byte(&tmp, W_TYPE_I64); w_byte(&tmp, W_TYPE_I64);
	w_uleb(&tmp, 1); w_byte(&tmp, W_TYPE_I64);
	w_byte(&tmp, W_TYPE_FUNC);
	w_uleb(&tmp, 3); w_byte(&tmp, W_TYPE_I64); w_byte(&tmp, W_TYPE_I64); w_byte(&tmp, W_TYPE_I64);
	w_uleb(&tmp, 1); w_byte(&tmp, W_TYPE_I64);
	w_section(m, 1, &tmp);

	/* Imports: the helpers, the shared 64-bit memory, the process's function table. */
	tmp.len = 0;
	w_uleb(&tmp, N_HELPERS + 2);
	for (int h = 0; h < N_HELPERS; h++) {
		w_name(&tmp, "rv");
		w_name(&tmp, helpers[h]);
		w_byte(&tmp, 0x00);
		w_uleb(&tmp, h >= H_FP ? 2 : 1);
	}
	w_name(&tmp, "env");
	w_name(&tmp, "memory");
	w_byte(&tmp, 0x02);
	w_byte(&tmp, 0x07);		/* has a maximum, shared, 64-bit */
	w_uleb(&tmp, 0);
	w_uleb(&tmp, 0x40000);		/* what programs on Linux/Wasm are linked with */
	w_name(&tmp, "env");
	w_name(&tmp, "table");
	w_byte(&tmp, 0x01);
	w_byte(&tmp, 0x70);		/* of function references */
	w_byte(&tmp, 0x00);		/* no maximum */
	w_uleb(&tmp, 0);
	w_section(m, 2, &tmp);
	if (tmp.overflow)
		m->overflow = 1;
}

/*
 * RV_JIT_OLD=bits, for measuring what this branch's changes do, side by side
 * in one machine: OLD_UNSHARED, every module of a thread different from every
 * other thread's (a custom section with the thread's id: no sharing of
 * compiled code by the engine), OLD_GROW_FLUSH, everything discarded when the
 * map grows, OLD_PROBES2, chaining looks at two places of the map.
 */
#define OLD_UNSHARED	1
#define OLD_GROW_FLUSH	2
#define OLD_PROBES2	4
#define OLD_BATCH_FIXED	8	/* a batch ends after JIT_BATCH_MIN + JIT_BATCH_MASK regions, whatever they are */

static void gen_unshared(struct wbuf *m)
{
	uint8_t data[32];
	struct wbuf tmp = { data, 0, sizeof(data), 0 };

	w_name(&tmp, "tid");
	w_uleb(&tmp, (uint64_t)syscall(SYS_gettid));
	w_section(m, 0, &tmp);
}

/*
 * RV_JIT_NAMES=1: the functions of a module get names (a "name" section at
 * its end) that say where their code is from, so that the host's profiler
 * shows translated code by region instead of as so many "r":
 * jPID_DEV:INODE_OFFSET for code of a file (the numbers as in RV_JIT_LOG, in
 * hex; the offset is in the file), jPID_mPC for code in no file (made by the
 * program: a JavaScript engine's). The reservation function is "reserve".
 * `pcs[j]` is the first address of the region whose function has index
 * `index[j]`.
 */
static void gen_names(struct rv_cpu *cpu, struct wbuf *m, const uint64_t *pcs, const uint32_t *index, uint32_t k)
{
	uint8_t sub_data[96 * (JIT_BATCH_MAX + 2)], tmp_data[sizeof(sub_data) + 16];
	struct wbuf sub = { sub_data, 0, sizeof(sub_data), 0 }, tmp = { tmp_data, 0, sizeof(tmp_data), 0 };
	uint32_t reserve = F_RESERVE;
	int done = 0;

	/* Names in the order of the functions' indices: the reservation function is N_HELPERS + 1, xlat after it. */
	w_uleb(&sub, k + N_MODULE_FNS);
	for (uint32_t j = 0; j < k; j++) {
		uint64_t dev, ino, offset;
		int fixed;
		char name[80];

		if (!done && index[j] > reserve) {
			w_uleb(&sub, reserve);
			w_name(&sub, "reserve");
			w_uleb(&sub, F_XLAT);
			w_name(&sub, "xlat");
			done = 1;
		}
		if (rv_mm_file_at(cpu, pcs[j], &dev, &ino, &offset, &fixed))
			snprintf(name, sizeof(name), "j%d_%llx:%llx_%llx", (int)getpid(), (unsigned long long)dev,
				 (unsigned long long)ino, (unsigned long long)offset);
		else
			snprintf(name, sizeof(name), "j%d_m%llx", (int)getpid(), (unsigned long long)pcs[j]);
		w_uleb(&sub, index[j]);
		w_name(&sub, name);
	}
	if (!done) {
		w_uleb(&sub, reserve);
		w_name(&sub, "reserve");
		w_uleb(&sub, F_XLAT);
		w_name(&sub, "xlat");
	}
	w_name(&tmp, "name");
	w_byte(&tmp, 1);		/* function names */
	w_uleb(&tmp, sub.len);
	w_bytes(&tmp, sub.data, sub.len);
	if (sub.overflow || tmp.overflow)
		return;			/* no names, then */
	w_section(m, 0, &tmp);
}

/*
 * OFF_HINTS: the branch hints section (the branch hinting proposal's
 * "metadata.code.branch_hint"), which has to come before the code section:
 * for each function that has hints, in the order of their indices, its index
 * and its hints, each the offset of an `if` or `br_if` from the start of the
 * function's code (its locals' declaration, which is where gen_module()'s
 * offsets count from), the length 1 and whether the branch is likely.
 * Engines that do not know the section skip it.
 */
static void gen_hints(struct wbuf *m, uint32_t nfn, const uint32_t *index, uint32_t *const *hints, const uint32_t *nhints)
{
	size_t cap = 64;
	uint32_t with = 0;
	struct wbuf sec;

	for (uint32_t j = 0; j < nfn; j++) {
		cap += 16 + 8 * (size_t)nhints[j];
		with += nhints[j] != 0;
	}
	if (!with)
		return;
	sec = (struct wbuf){ malloc(cap), 0, cap, 0 };
	if (!sec.data)
		return;		/* none, then */
	w_name(&sec, "metadata.code.branch_hint");
	w_uleb(&sec, with);
	for (uint32_t j = 0; j < nfn; j++) {
		if (!nhints[j])
			continue;
		w_uleb(&sec, index[j]);
		w_uleb(&sec, nhints[j]);
		for (uint32_t h = 0; h < nhints[j]; h++) {
			w_uleb(&sec, hints[j][h] >> 1);
			w_uleb(&sec, 1);
			w_byte(&sec, hints[j][h] & 1);
		}
	}
	if (!sec.overflow)
		w_section(m, 0, &sec);
	free(sec.data);
}

/* The region's function, in jit->func (gen_body() into jit->body first). jit->module.overflow is set if either did not fit. */
static void gen_func(struct rv_cpu *cpu, struct rv_jit *jit)
{
	struct gen g = { .cpu = cpu, .b = &jit->body, .r = jit->region, .lay = jit->layout };
	struct wbuf *f = &jit->func, *m = &jit->module;

	uint32_t n_access = 0;		/* the fourth group of locals (L_HOW on, or the translation cache's) */
	uint32_t n_fl;			/* the fifth: fcsr and the f registers (OFF_FPLOC) */
	int pcache_on;
	size_t at_body;

	pcache_on = jit->pcache_slots && !(jit->off & OFF_PCACHE) && !jit->flatmem_on && !jit->tag;
	/* The translation cache and the per-block groups both keep page checks in locals; one at a time (the cache's locals take the groups' places). */
	g.hoist = !(jit->off & OFF_HOIST) && !pcache_on;
	g.hints = !(jit->off & OFF_HINTS);
	jit->nhints = 0;
	m->overflow = 0;
	jit->body.len = jit->body.overflow = 0;
	cache_slots(&g);
	if (pcache_on)
		n_access = 3 * PCACHE_SLOTS;
	else if (g.hoist)
		n_access = L_CDELTA(CACHE_SLOTS - 1) + 1 - L_HOW;
	/* The floating-point group of locals follows the fourth: fcsr, then the f registers as they are used. */
	g.floc = !(jit->off & OFF_FPLOC) && !(jit->off & OFF_FP);
	g.fbase = L_HOW + n_access;
	memset(g.fmap, -1, sizeof(g.fmap));
	gen_body(&g);
	n_fl = g.floc && (g.nf || g.fc_used) ? 1 + (uint32_t)g.nf : 0;

	/* The function: locals, register loads, the body, register stores. */
	f->len = f->overflow = 0;
	w_uleb(f, 3 + (n_access ? 1 : 0) + (n_fl ? 1 : 0));
	w_uleb(f, N_I64_LOCALS);
	w_byte(f, W_TYPE_I64);
	w_uleb(f, N_I32_LOCALS);
	w_byte(f, W_TYPE_I32);
	w_uleb(f, N_I64_MORE);
	w_byte(f, W_TYPE_I64);
	if (n_access) {
		w_uleb(f, n_access);
		w_byte(f, W_TYPE_I64);
	}
	if (n_fl) {
		w_uleb(f, n_fl);
		w_byte(f, W_TYPE_I64);
	}

	/*
	 * The page table and its size are the process's, read from *cpu here
	 * rather than written into the code: so the code has no address of the
	 * process's in it, and the same region made by another process (a
	 * library at the same address) is the same bytes. (The table may be
	 * replaced while the region runs, as it could when its address was in
	 * the code: everything translated is discarded then, see the top, and
	 * the region is asked to leave at the end of its block.)
	 */
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, map));
	w_local(f, W_LOCAL_SET, L_MAP);
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, npages));
	w_local(f, W_LOCAL_SET, L_NPAGES);

	for (uint32_t r = 1; r < 32; r++) {
		if (!(g.used & (1u << r)))
			continue;
		w_local(f, W_LOCAL_GET, L_CPU);
		w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, x) + 8 * r);
		w_local(f, W_LOCAL_SET, L_REG(r));
	}

	if (n_fl) {
		if (g.fc_used) {
			w_local(f, W_LOCAL_GET, L_CPU);
			w_mem(f, W_I32_LOAD, 2, offsetof(struct rv_cpu, fcsr));
			w_op(f, W_I64_EXTEND_I32_U);
			w_local(f, W_LOCAL_SET, g.fbase);
		}
		for (uint32_t r = 0; r < 32; r++) {
			if (g.fmap[r] < 0)
				continue;
			w_local(f, W_LOCAL_GET, L_CPU);
			w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, f) + 8 * r);
			w_local(f, W_LOCAL_SET, g.fbase + 1 + (uint32_t)g.fmap[r]);
		}
	}
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, pc));
	w_local(f, W_LOCAL_SET, L_PC);
	/* The translation caches start out empty (a local starts as 0, which is page 0, never mapped; but be sure). */
	for (int k = 0; k < PCACHE_SLOTS; k++) {
		if (!(g.bases & (1u << k)))
			continue;
		w_i64(f, CACHE_NONE);
		w_local(f, W_LOCAL_SET, L_CB_R(k));
		w_i64(f, CACHE_NONE);
		w_local(f, W_LOCAL_SET, L_CB_W(k));
	}
	/* cpu->jit_used[a hash of the region's address] = 1: it was entered (evict(), used_hash()). */
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, jit_used));
	w_i64(f, 1);
	w_mem(f, W_I64_STORE8, 0, used_hash(jit->region->blocks[0].pc));
	w_local(f, W_LOCAL_GET, L_ENTRY);
	w_local(f, W_LOCAL_SET, L_NEXT);
	w_local(f, W_LOCAL_GET, L_BUDGET_IN);
	w_local(f, W_LOCAL_SET, L_BUDGET);

	at_body = f->len;
	w_bytes(f, jit->body.data, jit->body.len);
	for (uint32_t h = 0; h < jit->nhints; h++)
		jit->hints[h] += (uint32_t)at_body << 1;	/* offsets in the function */

	for (uint32_t r = 1; r < 32; r++) {
		if (!(g.written & (1u << r)))
			continue;
		w_local(f, W_LOCAL_GET, L_CPU);
		w_local(f, W_LOCAL_GET, L_REG(r));
		w_mem(f, W_I64_STORE, 3, offsetof(struct rv_cpu, x) + 8 * r);
	}
	if (n_fl) {
		if (g.fc_written) {
			w_local(f, W_LOCAL_GET, L_CPU);
			w_local(f, W_LOCAL_GET, g.fbase);
			w_op(f, W_I32_WRAP_I64);
			w_mem(f, W_I32_STORE, 2, offsetof(struct rv_cpu, fcsr));
		}
		for (uint32_t r = 0; r < 32; r++) {
			if (!(g.fwritten & (1u << r)))
				continue;
			w_local(f, W_LOCAL_GET, L_CPU);
			w_local(f, W_LOCAL_GET, g.fbase + 1 + (uint32_t)g.fmap[r]);
			w_mem(f, W_I64_STORE, 3, offsetof(struct rv_cpu, f) + 8 * r);
		}
	}
	w_local(f, W_LOCAL_GET, L_CPU);
	w_local(f, W_LOCAL_GET, L_PC);
	w_mem(f, W_I64_STORE, 3, offsetof(struct rv_cpu, pc));
	/* insn_count += the budget it was given - $budget */
	w_local(f, W_LOCAL_GET, L_CPU);
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, insn_count));
	w_local(f, W_LOCAL_GET, L_BUDGET_IN);
	w_local(f, W_LOCAL_GET, L_BUDGET);
	w_op(f, W_I32_SUB);
	w_op(f, W_I64_EXTEND_I32_S);
	w_op(f, W_I64_ADD);
	w_mem(f, W_I64_STORE, 3, offsetof(struct rv_cpu, insn_count));

	/*
	 * Chaining. If the region was left for a jump target and one of the
	 * map's first entries for that address (where map_find() looks first)
	 * is the address, a region has a block there: jump straight to it instead of
	 * going back to the dispatcher. The state is all in *cpu by now, and
	 * the callee gets what is left of the budget, so the dispatcher still
	 * gets control back as often as it did. (An exit for a pending signal
	 * or a spent budget is not for a jump target, so that is not chained.)
	 */
	w_local(f, W_LOCAL_GET, L_CHAIN);
	w_byte(f, W_IF);
	w_byte(f, W_TYPE_VOID);
	/* (The map is the thread's: where it is and its size are read through cpu->jit, so the code is the same in every thread.) */
	w_local(f, W_LOCAL_GET, L_CPU);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_cpu, jit));
	w_local(f, W_LOCAL_TEE, L_HOST);
	w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_jit, map));
	w_local(f, W_LOCAL_GET, L_PC);
	w_i64(f, 1);
	w_op(f, W_I64_SHR_U);
	w_i64(f, HASH_MULTIPLIER);
	w_op(f, W_I64_MUL);
	w_i64(f, 64);
	w_local(f, W_LOCAL_GET, L_HOST);
	w_mem(f, W_I32_LOAD, 2, offsetof(struct rv_jit, map_bits));
	w_op(f, W_I64_EXTEND_I32_U);
	w_op(f, W_I64_SUB);
	w_op(f, W_I64_SHR_U);
	w_i64(f, 4);			/* times sizeof(struct jit_entry) */
	w_op(f, W_I64_SHL);
	w_op(f, W_I64_ADD);
	w_local(f, W_LOCAL_SET, L_ADDR);
	/* The entry the address hashes to, then the next one, where a collision would have put it. */
	for (uint64_t probe = 0; probe < ((jit->old & OLD_PROBES2) ? 2 : MAP_PROBES); probe++) {
		uint64_t entry = probe * sizeof(struct jit_entry);

		w_local(f, W_LOCAL_GET, L_ADDR);
		w_mem(f, W_I64_LOAD, 3, entry + offsetof(struct jit_entry, pc));
		w_local(f, W_LOCAL_GET, L_PC);
		w_op(f, W_I64_EQ);
		w_byte(f, W_IF);
		w_byte(f, W_TYPE_VOID);
		if (cpu->verbose) {
			w_local(f, W_LOCAL_GET, L_HOST);
			w_local(f, W_LOCAL_GET, L_HOST);
			w_mem(f, W_I64_LOAD, 3, offsetof(struct rv_jit, stat_chained));
			w_i64(f, 1);
			w_op(f, W_I64_ADD);
			w_mem(f, W_I64_STORE, 3, offsetof(struct rv_jit, stat_chained));
		}
		w_local(f, W_LOCAL_GET, L_CPU);
		w_local(f, W_LOCAL_GET, L_ADDR);
		w_mem(f, W_I32_LOAD, 2, entry + offsetof(struct jit_entry, index));
		w_local(f, W_LOCAL_GET, L_BUDGET);
		w_local(f, W_LOCAL_GET, L_ADDR);
		w_mem(f, W_I32_LOAD, 2, entry + offsetof(struct jit_entry, slot));
		w_byte(f, W_RETURN_CALL_INDIRECT);
		w_uleb(f, 0);			/* type 0: a region */
		w_uleb(f, 0);			/* the process's function table */
		w_op(f, W_END);
	}
	w_op(f, W_END);
	w_op(f, W_END);
	if (jit->body.overflow || f->overflow || g.bad)
		m->overflow = 1;
}

/* The region's module, in jit->module, around its function, in jit->func (gen_func(), or the store's: cache_find()). */
static void gen_wrap(struct rv_cpu *cpu, struct rv_jit *jit)
{
	struct wbuf *f = &jit->func, *m = &jit->module;
	static uint8_t tmp_data[1024];	/* the small sections, one at a time */
	struct wbuf tmp = { tmp_data, 0, sizeof(tmp_data), 0 };

	m->len = m->overflow = 0;
	gen_module_head(m);

	tmp.len = 0;
	w_uleb(&tmp, 1 + N_MODULE_FNS);
	w_uleb(&tmp, 0);	/* the region */
	w_uleb(&tmp, 2);	/* reserve */
	w_uleb(&tmp, 2);	/* xlat */
	w_section(m, 3, &tmp);

	tmp.len = 0;
	w_uleb(&tmp, 1);
	w_name(&tmp, "r");
	w_byte(&tmp, 0x00);
	w_uleb(&tmp, N_HELPERS);
	w_section(m, 7, &tmp);

	{
		uint32_t index = F_REGION, *hints = jit->hints;

		gen_hints(m, 1, &index, &hints, &jit->nhints);
	}

	/* Code: the bodies, each preceded by its size. */
	{
		uint8_t size_data[16], fn_data[512], fn_size_data[16], xl_data[512], xl_size_data[16];
		struct wbuf size = { size_data, 0, sizeof(size_data), 0 };
		struct wbuf fn = { fn_data, 0, sizeof(fn_data), 0 };
		struct wbuf fn_size = { fn_size_data, 0, sizeof(fn_size_data), 0 };
		struct wbuf xl = { xl_data, 0, sizeof(xl_data), 0 };
		struct wbuf xl_size = { xl_size_data, 0, sizeof(xl_size_data), 0 };

		gen_reserve_fn(cpu, &fn);
		gen_xlat_fn(&xl);
		w_uleb(&size, f->len);
		w_uleb(&fn_size, fn.len);
		w_uleb(&xl_size, xl.len);
		w_byte(m, 10);
		w_uleb(m, 1 + size.len + f->len + fn_size.len + fn.len + xl_size.len + xl.len);
		w_uleb(m, 1 + N_MODULE_FNS);
		w_bytes(m, size.data, size.len);
		w_bytes(m, f->data, f->len);
		w_bytes(m, fn_size.data, fn_size.len);
		w_bytes(m, fn.data, fn.len);
		w_bytes(m, xl_size.data, xl_size.len);
		w_bytes(m, xl.data, xl.len);
		if (fn.overflow || xl.overflow)
			m->overflow = 1;
	}

	if (jit->names) {
		uint64_t pc = jit->region->blocks[0].pc;
		uint32_t index = F_REGION;

		gen_names(cpu, m, &pc, &index, 1);
	}
	if (jit->old & OLD_UNSHARED)
		gen_unshared(m);

	if (f->overflow || tmp.overflow)
		m->overflow = 1;
}

/* ---------------------------------------------------------------- driving */

/* A hash of a function's code (the first half of the store's key for it, jitcache.c). */
static uint64_t body_hash(const uint8_t *code, size_t len)
{
	uint8_t key[RV_JC_KEY];
	uint64_t h;

	rv_jc_key(code, len, key);
	memcpy(&h, key, sizeof(h));
	return h;
}

static long install(struct rv_jit *jit, uint64_t entry_pc, long slot)
{
	if (jit->dump_dir) {
		char path[512];
		FILE *out;

		snprintf(path, sizeof(path), "%s/region-%llx.wasm", jit->dump_dir, (unsigned long long)entry_pc);
		out = fopen(path, "wb");
		if (out) {
			fwrite(jit->module.data, 1, jit->module.len, out);
			fclose(out);
		}
		/* Its layout (struct layout), for looking at. */
		snprintf(path, sizeof(path), "%s/region-%llx.layout", jit->dump_dir, (unsigned long long)entry_pc);
		out = fopen(path, "w");
		if (out) {
			const struct layout *lay = jit->layout;

			for (int sc = 0; sc < lay->nscopes; sc++) {
				fprintf(out, "scope %d: parent %d item %d header %d lookup %d:", sc, lay->scopes[sc].parent,
					lay->scopes[sc].parent_item, lay->scopes[sc].header, lay->scopes[sc].lookup);
				for (int k = 0; k < lay->scopes[sc].nitems; k++) {
					int v = lay->items[lay->scopes[sc].first + k];

					if (v >= 0)
						fprintf(out, " b%d@%llx", v, (unsigned long long)jit->region->blocks[v].pc);
					else if (v == ITEM_LOOKUP)
						fprintf(out, " lookup");
					else
						fprintf(out, " scope%d", SCOPE_OF_ITEM(v));
				}
				fprintf(out, "\n");
			}
			for (int v = 0; v < jit->region->nblocks; v++) {
				fprintf(out, "b%d@%llx ->", v, (unsigned long long)jit->region->blocks[v].pc);
				for (int w = 0; w < jit->region->nblocks; w++)
					if (in_set(lay->succ[v], w))
						fprintf(out, " b%d", w);
				fprintf(out, "\n");
			}
			fclose(out);
		}
		/* And which blocks it has, in order, for tools/region-bench.js. */
		snprintf(path, sizeof(path), "%s/region-%llx.blocks", jit->dump_dir, (unsigned long long)entry_pc);
		out = fopen(path, "w");
		if (out) {
			for (int k = 0; k < jit->region->nblocks; k++)
				fprintf(out, "%llx %u\n", (unsigned long long)jit->region->blocks[k].pc, jit->region->blocks[k].count);
			fclose(out);
		}
	}
#ifdef __wasm__
	return __rv_jit_install(jit->module.data, jit->module.len, slot);
#else
	/* No Wasm engine here: the module can only be dumped, or run by wasm_emu.c (RV_JIT_EMU). (RV_JIT_FAKE: as if it had a slot, see the top.) */
	if (jit->emu)
		return rv_emu_install(jit->module.data, jit->module.len, slot);
	if (jit->fake)
		return slot >= 0 ? slot : (long)jit->fake_slot++;
	return -1;
#endif
}

/*
 * What a region is next to, so that regions near each other in the guest's code
 * (the same file, the same 2^shift bytes of it) make a batch together and no
 * others: a batch is then the regions of one stretch of code that came before
 * its end, not whichever regions happened to come at about the same time, and
 * a program started again, whose regions come in another order (from the
 * store, at once, instead of after being run a few hundred times), makes the
 * same batches for the stretches it runs the same. Code that is in no file
 * (a JavaScript engine's) is by address. shift 64: one bucket for everything.
 */
static uint64_t batch_bucket(struct rv_cpu *cpu, const struct rv_jit *jit, uint64_t pc)
{
	uint64_t dev, ino, offset, h = 0x9e3779b97f4a7c15ULL;
	int fixed;

	if (jit->group_shift >= 64)
		return 0;
	if (rv_mm_file_at(cpu, pc, &dev, &ino, &offset, &fixed)) {
		h = (h ^ dev) * 0x100000001b3ULL;
		h = (h ^ ino) * 0x100000001b3ULL;
		h = (h ^ (offset >> jit->group_shift)) * 0x100000001b3ULL;
	} else {
		h = (h ^ 0xff) * 0x100000001b3ULL;
		h = (h ^ (pc >> jit->group_shift)) * 0x100000001b3ULL;
	}
	return h | 1;
}

static void batch_make(struct rv_cpu *cpu, struct rv_jit *jit, uint64_t bucket);

/* A new region (made[n], already in pending) has come: does a batch of its bucket end with it? */
static void batch_trigger(struct rv_cpu *cpu, struct rv_jit *jit, uint32_t n)
{
	uint32_t count = 0;
	uint64_t bucket = jit->made[n].bucket;

	for (uint32_t i = 0; i < jit->npending; i++)
		count += jit->made[jit->pending[i]].bucket == bucket;
	if (count >= JIT_BATCH_MAX ||
	    (count >= jit->batch_min &&
	     ((jit->old & OLD_BATCH_FIXED) ? count >= jit->batch_min + jit->batch_mask :
	      (jit->made[n].body_hash & jit->batch_mask) == 0))) {
		batch_make(cpu, jit, bucket);
	} else if (jit->npending >= JIT_PENDING_MAX) {
		batch_make(cpu, jit, jit->made[jit->pending[0]].bucket);
	}
}

/*
 * The regions waiting (jit->pending) as one module: their functions again,
 * as they were made, and the reservation function, each region's function put
 * into its region's table slot by an element segment when the module is
 * instantiated, in place of its module of its own, which the engine then
 * collects. A region's function calls the reservation function by its index,
 * N_HELPERS + 1 (F_RESERVE), so that is where it is here too: the first
 * region's function, the reservation function, then the others. Regions
 * call one another only through the table. The host is given the slots after
 * the module (__rv_jit_install with slot -16 - count) and puts function "j"
 * of the module's exports into the j-th.
 */
static void batch_make(struct rv_cpu *cpu, struct rv_jit *jit, uint64_t bucket)
{
	uint32_t k = 0, list[JIT_BATCH_MAX], keep = 0;
	size_t cap = 2048;
	struct wbuf m, fn, xl, sec;
	uint8_t fn_data[512], xl_data[512];
	long got;

	for (uint32_t i = 0; i < jit->npending; i++) {
		const struct jit_made *r = &jit->made[jit->pending[i]];

		if (r->bucket == bucket && k < JIT_BATCH_MAX) {
			if (r->state != R_FREE && r->body) {
				list[k++] = jit->pending[i];
				cap += r->body_len + 32 + 8 * (size_t)r->nhints;
			}
		} else {
			jit->pending[keep++] = jit->pending[i];	/* (waits for its own bucket's batch) */
		}
	}
	jit->npending = keep;
	if (k < 1 || (k < 2 && jit->group_shift >= 64))
		goto out;
	/* (The functions in the order of their hashes: the same regions are the same bytes whatever came first.) */
	for (uint32_t i = 1; i < k; i++) {
		uint32_t x = list[i], j = i;

		while (j && (jit->made[list[j - 1]].body_hash > jit->made[x].body_hash ||
			     (jit->made[list[j - 1]].body_hash == jit->made[x].body_hash && list[j - 1] > x))) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = x;
	}
	if (jit->log_fd >= 0) {
		/* RV_JIT_LOG: B pid regions hash:length... (in the order of the batch) */
		char line[JIT_BATCH_MAX * 26 + 40];
		int n = snprintf(line, sizeof(line), "B %d %u", (int)getpid(), k);

		for (uint32_t j = 0; j < k && n > 0 && (size_t)n < sizeof(line) - 30; j++)
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %016llx:%u",
				      (unsigned long long)jit->made[list[j]].body_hash, jit->made[list[j]].body_len);
		if (n > 0 && (size_t)n < sizeof(line) - 2) {
			line[n++] = '\n';
			if (write(jit->log_fd, line, (size_t)n) < 0)
				jit->log_fd = -1;
		}
	}
	if (jit->nbatches == jit->batches_cap) {
		uint32_t bcap = jit->batches_cap ? jit->batches_cap * 2 : 64;
		uint16_t *live = realloc(jit->batch_live, bcap * sizeof(*live)), *size;

		if (live)
			jit->batch_live = live;
		size = live ? realloc(jit->batch_size, bcap * sizeof(*size)) : NULL;
		if (!size) {
			jit->stat_nomem++;
			goto out;
		}
		jit->batch_size = size;
		jit->batches_cap = bcap;
	}
	if (jit->names)
		cap += 96 * (k + 2);
	m = (struct wbuf){ malloc(cap + 4 * k), 0, cap, 0 };
	sec = (struct wbuf){ malloc(cap), 0, cap, 0 };
	if (!m.data || !sec.data) {
		free(m.data);
		free(sec.data);
		jit->stat_nomem++;
		goto out;
	}
	fn = (struct wbuf){ fn_data, 0, sizeof(fn_data), 0 };
	xl = (struct wbuf){ xl_data, 0, sizeof(xl_data), 0 };
	gen_reserve_fn(cpu, &fn);
	gen_xlat_fn(&xl);
	gen_module_head(&m);

	/* Functions: region, reservation, xlat, regions. */
	sec.len = 0;
	w_uleb(&sec, k + N_MODULE_FNS);
	w_uleb(&sec, 0);
	w_uleb(&sec, 2);
	w_uleb(&sec, 2);
	for (uint32_t j = 1; j < k; j++)
		w_uleb(&sec, 0);
	w_section(&m, 3, &sec);

	/*
	 * Exports: function j as "j", which the host puts into the slot of
	 * region j (the slots follow the module). Not element segments with
	 * the slots in them: without them the module is the same bytes in
	 * every process that makes the same batch (a program started again),
	 * and the engine compiles it once.
	 */
	sec.len = 0;
	w_uleb(&sec, k);
	for (uint32_t j = 0; j < k; j++) {
		char name[12];

		snprintf(name, sizeof(name), "%u", j);
		w_name(&sec, name);
		w_byte(&sec, 0x00);		/* a function */
		w_uleb(&sec, j ? N_HELPERS + N_MODULE_FNS + j : N_HELPERS);
	}
	w_section(&m, 7, &sec);

	{
		uint32_t index[JIT_BATCH_MAX], *hints[JIT_BATCH_MAX], nhints[JIT_BATCH_MAX];

		for (uint32_t j = 0; j < k; j++) {
			index[j] = j ? N_HELPERS + 1 + j : N_HELPERS;
			hints[j] = jit->made[list[j]].hints;
			nhints[j] = jit->made[list[j]].nhints;
		}
		gen_hints(&m, k, index, hints, nhints);
	}

	/* Code: the bodies in the same order, each preceded by its size. */
	sec.len = 0;
	w_uleb(&sec, k + N_MODULE_FNS);
	for (uint32_t j = 0; j < k; j++) {
		const struct jit_made *r = &jit->made[list[j]];

		w_uleb(&sec, r->body_len);
		w_bytes(&sec, r->body, r->body_len);
		if (!j) {
			w_uleb(&sec, fn.len);
			w_bytes(&sec, fn.data, fn.len);
			w_uleb(&sec, xl.len);
			w_bytes(&sec, xl.data, xl.len);
		}
	}
	w_section(&m, 10, &sec);
	free(sec.data);
	if (jit->names) {
		uint64_t pcs[JIT_BATCH_MAX];
		uint32_t index[JIT_BATCH_MAX];

		for (uint32_t j = 0; j < k; j++) {
			pcs[j] = jit->made[list[j]].pcs[0];
			index[j] = j ? N_HELPERS + N_MODULE_FNS + j : N_HELPERS;
		}
		gen_names(cpu, &m, pcs, index, k);
	}
	if (jit->old & OLD_UNSHARED)
		gen_unshared(&m);
	if (m.overflow || sec.overflow || fn.overflow || xl.overflow) {
		free(m.data);
		jit->stat_batch_failed++;
		goto out;
	}
	/* The slots, after the module. */
	for (uint32_t j = 0; j < k; j++)
		memcpy(m.data + m.len + 4 * j, &jit->slots[list[j]], 4);

	if (jit->dump_dir) {
		char path[512];
		FILE *out;

		snprintf(path, sizeof(path), "%s/batch-%llu.wasm", jit->dump_dir, (unsigned long long)jit->stat_batches);
		if ((out = fopen(path, "wb"))) {
			fwrite(m.data, 1, m.len, out);
			fclose(out);
		}
	}
#ifdef __wasm__
	got = __rv_jit_install(m.data, m.len, -16 - (long)k);
#else
	got = jit->emu ? rv_emu_install(m.data, m.len, -16 - (long)k) : jit->fake ? 0 : -1;
#endif
	if (got < 0) {
		free(m.data);
		jit->stat_batch_failed++;
		goto out;
	}
	{
		uint32_t b = jit->nbatches++, share = (code_kib(m.len) + k - 1) / k;

		jit->batch_live[b] = jit->batch_size[b] = (uint16_t)k;
		for (uint32_t j = 0; j < k; j++) {
			struct jit_made *r = &jit->made[list[j]];

			r->batch = b + 1;
			jit->code_kib = jit->code_kib - r->kib + share;
			r->kib = share;
		}
	}
	free(m.data);
	jit->stat_batches++;
	jit->stat_batched += k;
out:
	for (uint32_t j = 0; j < k; j++) {
		free(jit->made[list[j]].body);	/* (and its hints) */
		jit->made[list[j]].body = NULL;
		jit->made[list[j]].hints = NULL;
		jit->made[list[j]].nhints = 0;
	}
	/* (And those left out: not in a batch, and not to be.) */
}

static uint32_t target_slot(uint64_t pc, uint64_t multiplier, uint32_t bits)
{
	return (uint32_t)(((pc >> 1) * multiplier) >> (64 - bits));
}

/* Fill the table (1 << bits entries) using this multiplier, return addresses first. Returns a cost: what did not fit. */
static uint32_t fill_targets(struct jit_target *table, uint32_t bits, const struct jit_region *r, uint64_t multiplier)
{
	uint32_t missed = 0;

	for (uint32_t i = 0; i < 1u << bits; i++)
		table[i].pc = TARGET_NONE;
	for (int pass = 0; pass < 2; pass++) {
		for (int k = 0; k < r->nblocks; k++) {
			struct jit_target *t = &table[target_slot(r->blocks[k].pc, multiplier, bits)];

			if (r->blocks[k].is_return != !pass)
				continue;
			if (t->pc == TARGET_NONE) {
				t->pc = r->blocks[k].pc;
				t->index = (uint64_t)k;
			} else {
				missed += pass ? 1 : 100;
			}
		}
	}
	return missed;
}

/*
 * The table of the region's blocks for its indirect jumps, if it has any, in
 * jit->targets_now. It belongs to the region's number n (which the caller
 * has made room for), and goes when the region does. 0, or -1 without memory.
 */
static int target_table(struct rv_jit *jit, uint32_t n, const struct jit_region *r)
{
	uint32_t best_missed = ~0u, bits = TARGET_BITS_MIN;
	struct jit_target *table;
	int jumps = 0;

	free(jit->targets[n]);	/* (a number that is free has none; but to be sure) */
	jit->targets[n] = jit->targets_now = NULL;
	for (int k = 0; k < r->nblocks && !jumps; k++)
		jumps = OPCODE(r->insns[r->blocks[k].first + r->blocks[k].count - 1].raw) == 0x67;
	if (!jumps || (jit->off & OFF_TARGETS))
		return 0;
	while (bits < TARGET_BITS && (1u << bits) < 2u * (uint32_t)r->nblocks)
		bits++;
	if (!(table = malloc(sizeof(*table) << bits)))
		return -1;

	/* Return addresses weigh most: they are what indirect jumps mostly go to. */
	for (int try = 0; try < TARGET_TRIES && best_missed; try++) {
		uint64_t multiplier = HASH_MULTIPLIER * (2 * (uint64_t)try + 1);
		uint32_t missed = fill_targets(table, bits, r, multiplier);

		if (missed < best_missed) {
			best_missed = missed;
			jit->target_multiplier = multiplier;
		}
	}
	fill_targets(table, bits, r, jit->target_multiplier);
	jit->targets[n] = jit->targets_now = table;
	jit->target_bits = bits;
	return 0;
}

/*
 * The buffers a translation is put together in: 3 MiB and a region's
 * description, which is too much to have per thread, and for a process that
 * never translates anything. One set for the process, made when first
 * needed; a thread has it for the length of one translation.
 */
static pthread_mutex_t scratch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jit_region *scratch_region;
static struct layout *scratch_layout;
static uint8_t *scratch_code;
static size_t scratch_cap;

/* With the lock: the three buffers, each of `cap` bytes. -1 if there is no memory. */
static int scratch_size(size_t cap)
{
	uint8_t *code = malloc(3 * cap);

	if (!code)
		return -1;
	free(scratch_code);
	scratch_code = code;
	scratch_cap = cap;
	return 0;
}

static int scratch_get(struct rv_cpu *cpu, struct rv_jit *jit)
{
	/* (Not pthread_mutex_lock(): the thread that has them may be parked, thread.c.) */
	rv_lock_other(cpu, &scratch_lock);
	if (!scratch_region && !(scratch_region = malloc(sizeof(*scratch_region)))) {
		pthread_mutex_unlock(&scratch_lock);
		return -1;
	}
	if (!scratch_layout && !(scratch_layout = malloc(sizeof(*scratch_layout)))) {
		pthread_mutex_unlock(&scratch_lock);
		return -1;
	}
	jit->layout = scratch_layout;
	if (!scratch_code && scratch_size(CODE_CAP_MIN)) {
		pthread_mutex_unlock(&scratch_lock);
		return -1;
	}
	jit->region = scratch_region;
	jit->body.data = scratch_code;
	jit->func.data = scratch_code + scratch_cap;
	jit->module.data = scratch_code + 2 * scratch_cap;
	jit->body.cap = jit->func.cap = jit->module.cap = scratch_cap;
	return 0;
}

static void scratch_put(void)
{
	pthread_mutex_unlock(&scratch_lock);
}

/*
 * In the child of the host's fork() (syscall.c), which has only the thread
 * that forked: another thread may have had the buffers when the world was
 * stopped for the fork (it parks with them, rv_lock_other()), and is not
 * there to give them back. Nobody else is, now. (Without this the child's
 * first translation waited for good: tools/thread-check.sh's threads-musl,
 * natively.)
 */
void rv_jit_after_fork(void)
{
	pthread_mutex_init(&scratch_lock, NULL);
}

static int translate_region(struct rv_cpu *cpu, uint64_t pc);

/*
 * Translate the region starting at pc. Returns 1 if pc can now be run from
 * the map, 0 if it cannot be translated, 2 if it was but code changed
 * somewhere meanwhile: the dispatcher has to look at that first.
 */
static int translate(struct rv_cpu *cpu, uint64_t pc)
{
	struct rv_jit *jit = cpu->jit;
	int done;

	rv_stack_note("translating");
	if (scratch_get(cpu, jit)) {
		jit->stat_nomem++;
		return 0;
	}
	/* A region too large for the buffers: larger buffers, and again. */
	while ((done = translate_region(cpu, pc)) < 0) {
		if (scratch_cap >= CODE_CAP_MAX || scratch_size(scratch_cap * 2)) {
			if (scratch_cap < CODE_CAP_MAX)
				jit->stat_nomem++;
			jit->stat_failed++;
			done = 0;
			break;
		}
		jit->body.data = scratch_code;
		jit->func.data = scratch_code + scratch_cap;
		jit->module.data = scratch_code + 2 * scratch_cap;
		jit->body.cap = jit->func.cap = jit->module.cap = scratch_cap;
	}
	scratch_put();
	/* (If it was a page this was made from: the mark was there, so the log says so, and the region is known by its pages.) */
	if (done && jit->epoch != cpu->proc->code_epoch)
		return 2;
	return done;
}

/*
 * RV_JIT_LOG: what was translated, a line per region, for finding out how
 * often different processes translate the same thing:
 *
 *   R pid device:inode offset base instructions blocks hash fixed ns-to-make ns-to-compile bytes tid same cached ns-build ns-store ns-function ns-module
 *
 * device:inode and offset say which file the region's first instruction is
 * of and where in it (0:0 if none), base is its address minus that offset
 * (where the file is loaded), hash is of the region's instructions and of
 * how its blocks lie relative to the first (not of where it is loaded),
 * fixed is 1 if the code is in pages that are the file's own and cannot have
 * been written, same is 1 if the module is, byte for byte, one that was made
 * in this process before (by this thread or another: module_seen()), which
 * the browser's engine then has compiled already, cached is 1 if its function was
 * found in the store of translations kept (cache_find()). And a line for every round of letting go of regions to make
 * room (see "Room for translated code"):
 *
 *   E pid tid regions KiB-let-go-of KiB-the-thread-keeps budget-KiB sum-KiB put-off hard
 *
 * budget and sum as the host last answered them (sum before this round),
 * put-off the translations this thread has put off for want of room so far,
 * hard 1 if regions still in use went as well.
 */
static void log_region(struct rv_cpu *cpu, const struct jit_region *r, uint64_t made_ns, uint64_t compiled_ns, uint64_t bytes,
		       int same, int cached)
{
	uint64_t pc = r->blocks[0].pc, dev = 0, ino = 0, offset = 0, hash = 0xcbf29ce484222325ULL;
	char line[300];
	int fixed = 0, n;

	for (int k = 0; k < r->nblocks; k++) {
		const struct jit_block *b = &r->blocks[k];
		uint64_t words[2] = { b->pc - pc, b->count };

		for (unsigned i = 0; i < sizeof(words); i++)
			hash = (hash ^ ((const uint8_t *)words)[i]) * 0x100000001b3ULL;
		for (uint32_t i = 0; i < b->count; i++) {
			uint32_t raw = r->insns[b->first + i].raw;

			for (unsigned j = 0; j < 4; j++)
				hash = (hash ^ ((raw >> (8 * j)) & 0xff)) * 0x100000001b3ULL;
		}
	}
	if (rv_mm_file_at(cpu, pc, &dev, &ino, &offset, &fixed)) {
		/* (All of it in such pages? The first and the last block's, at least.) */
		uint64_t d2, i2, o2;
		int fixed2 = 0;

		if (!rv_mm_file_at(cpu, r->blocks[r->nblocks - 1].pc, &d2, &i2, &o2, &fixed2) || !fixed2)
			fixed = 0;
	}
	n = snprintf(line, sizeof(line), "R %d %llx:%llx %llx %llx %d %d %016llx %d %llu %llu %llu %d %d %d %llu %llu %llu %llu %016llx %zu %llx\n", (int)getpid(),
		     (unsigned long long)dev, (unsigned long long)ino, (unsigned long long)offset,
		     (unsigned long long)(pc - offset), r->ninsns, r->nblocks, (unsigned long long)hash, fixed,
		     (unsigned long long)made_ns, (unsigned long long)compiled_ns, (unsigned long long)bytes, (int)cpu->tid, same, cached,
		     (unsigned long long)cpu->jit->lap[0], (unsigned long long)cpu->jit->lap[1],
		     (unsigned long long)cpu->jit->lap[2], (unsigned long long)cpu->jit->lap[3],
		     (unsigned long long)body_hash(cpu->jit->func.data, cpu->jit->func.len), cpu->jit->func.len, (unsigned long long)pc);
	if (n > 0 && write(cpu->jit->log_fd, line, (size_t)n) < 0)
		cpu->jit->log_fd = -1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Was a module of these bytes made in this process before? The hashes of the
 * modules made, in a table of SEEN_PLACES (with the scratch buffers' lock,
 * which whoever translates has), forgotten when it is half full. A module
 * that was is one the engine has compiled already, as long as some thread
 * still has it (its native module cache): that is how often threads make the
 * same translations.
 */
#define SEEN_BITS	16
#define SEEN_PLACES	(1u << SEEN_BITS)
static uint64_t *seen_table;
static uint32_t seen_used;

static int module_seen(const struct wbuf *m)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	uint32_t i;

	if (!seen_table && !(seen_table = calloc(SEEN_PLACES, sizeof(*seen_table))))
		return 0;
	for (size_t k = 0; k < m->len; k++)
		h = (h ^ m->data[k]) * 0x100000001b3ULL;
	h |= 1;
	for (i = (uint32_t)(h >> (64 - SEEN_BITS)); seen_table[i]; i = (i + 1) & (SEEN_PLACES - 1))
		if (seen_table[i] == h)
			return 1;
	if (seen_used >= SEEN_PLACES / 2) {
		memset(seen_table, 0, SEEN_PLACES * sizeof(*seen_table));
		seen_used = 0;
		i = (uint32_t)(h >> (64 - SEEN_BITS));
	}
	seen_table[i] = h;
	seen_used++;
	return 0;
}

/*
 * Translations kept (jitcache.c says what the store is, where it is and why
 * it is nowhere the guest can write). A program that starts translates the
 * same code that it, or another program using the same libraries, translated
 * the last time it ran, in this visit of the page or an earlier one. So a
 * region of a file's code (a program's, a library's) is kept in the store as
 * its function, under a key made from everything the function depends on
 * (cache_describe()), and a region is looked for there before it is made.
 *
 * What it depends on: where its blocks are (the code jumps to absolute guest
 * addresses, and a library is at the same address in every process that has
 * it, so this loses little: tests/jitcache.sh), the bytes of each instruction
 * as they were read, whether a call returns to a block, the switches that
 * change the code (RV_JIT_OFF, RV_JIT_OLD, -v, RV_JIT_TAG: cache_switches()), the host's fused multiply-add, and the
 * translator's own sources (RV_JIT_SOURCE, in every key). The description
 * is kept with the function and compared byte for byte before the function is
 * used, so a key two regions share by chance does no harm, and a file that
 * changed (a program built again) simply has other bytes: the description of
 * its regions is not the old one. Which file, its inode or its time are not
 * needed for that, and would not be enough.
 */
/*
 * THE ONE PLACE for run-time switches that change a region's function.
 * Every environment variable (or other setting) that the code generator reads
 * and that makes it write other bytes for the same region goes into
 * cache_switches() below, as one more word of cache_head.sw[] (there is room:
 * CACHE_SWITCHES words, unused ones are 0), or a record made with one setting
 * would be handed to a run with another. Not to be put there: what only
 * decides whether or when a region is made (RV_JIT_HOT, _WARM, _LIMIT, _HINTS,
 * _GROUP, _REFUSE), what is made around the function (RV_JIT_NAMES, _DUMP,
 * _LOG: they are in the module's wrapper or not in it) and the checks
 * (RV_JIT_CHECK, RV_JIT_CACHE_VERIFY), which compare. Known to come with the
 * branches that change the generator: RV_JIT_CHAIN (the length of a jump's
 * chain), RV_JIT_LINEAR, RV_JIT_EDGES (rvjit-safe, rvjit-dispatch): each adds
 * its line in cache_switches() when it lands. The generator's own sources are
 * not a switch: RV_JIT_SOURCE (every .c and .h of rv, rv/Makefile) is in every key.
 */
#define CACHE_SWITCHES	8
struct cache_head {
	uint32_t magic;
	uint32_t nblocks, ninsns;
	int32_t fma;
	uint32_t sw[CACHE_SWITCHES];
};
static void cache_switches(const struct rv_cpu *cpu, const struct rv_jit *jit, uint32_t *sw)
{
	sw[0] = jit->off;		/* RV_JIT_OFF: parts of the translator left out */
	sw[1] = jit->old;		/* RV_JIT_OLD: as before an earlier change, for measuring */
	sw[2] = cpu->verbose != 0;	/* -v: checks in the code */
	sw[3] = jit->tag != 0;		/* RV_JIT_TAG: code that says where it is from */
	/*
	 * The access path (jitmem): RV_JIT_OUTLINE=1 and RV_JIT_OFF's OFF_OUTLINE, OFF_HOIST, OFF_HINTS, all in
	 * jit->off already (sw[0]); said again here so that they stay keyed if sw[0] ever leaves bits out.
	 * OFF_HINTS does not change the function's bytes but what is kept with them (the branch hints).
	 */
	sw[4] = jit->off & (OFF_OUTLINE | OFF_HOIST | OFF_HINTS);
	sw[5] = jit->hint_sites;	/* RV_JIT_HINT_SITES: which hints are kept with the function */
	sw[6] = (uint32_t)jit->pcache_slots;	/* RV_JIT_PCACHE: registers with a translation cache */
	sw[7] = jit->flatmem_on ? (uint32_t)jit->flatmem | 1 : 0;	/* RV_JIT_FLATMEM (a measuring tool: no check at all) */
}
struct cache_block {
	uint64_t pc, next_pc;
	uint32_t first, count;
	uint32_t is_return, bytes;
};
#define CACHE_MAGIC	0x34636a72	/* "rjc4": a record is the description, the branch hints (a count, then each), then the function (not the module) */

/* What region r is made from, into d. 0, or -1 if it does not fit. */
static int cache_describe(struct rv_cpu *cpu, const struct jit_region *r, struct wbuf *d)
{
	struct rv_jit *jit = cpu->jit;
	struct cache_head head = { CACHE_MAGIC, (uint32_t)r->nblocks, (uint32_t)r->ninsns, 0, { 0 } };

	if (!jit->fma)
		jit->fma = fma_probe(jit);
	head.fma = jit->fma;
	cache_switches(cpu, jit, head.sw);
	d->len = d->overflow = 0;
	w_bytes(d, &head, sizeof(head));
	for (int k = 0; k < r->nblocks; k++) {
		const struct jit_block *b = &r->blocks[k];
		struct cache_block cb = { b->pc, b->next_pc, b->first, b->count, (uint32_t)b->is_return, 0 };

		for (uint32_t i = 0; i < b->count; i++)
			cb.bytes += r->insns[b->first + i].len;
		w_bytes(d, &cb, sizeof(cb));
		for (uint32_t i = 0; i < b->count; i++) {
			const struct jit_insn *insn = &r->insns[b->first + i];

			w_bytes(d, &insn->orig, insn->len);
		}
	}
	return d->overflow ? -1 : 0;
}

/*
 * The function for region r from the store, into jit->func: 1 if it is there
 * as made from exactly what r is made from, 0 if not. The description is
 * made in jit->body and the record read into jit->module meanwhile; the key
 * is left in `key`, for keeping the function made instead (cache_keep()).
 */
static int cache_find(struct rv_cpu *cpu, const struct jit_region *r, uint8_t key[RV_JC_KEY])
{
	struct rv_jit *jit = cpu->jit;
	struct wbuf *d = &jit->body, *m = &jit->module, *f = &jit->func;
	long len;

	if (cache_describe(cpu, r, d))
		return 0;
	rv_jc_key(d->data, d->len, key);
	len = rv_jc_get(key, m->data, m->cap);
	if (len < 0 || (size_t)len > m->cap || (size_t)len < d->len + 4 || memcmp(m->data, d->data, d->len)) {
		jit->stat_jc_misses++;
		return 0;
	}
	/* The branch hints kept with it (offsets in the function), then the function. */
	{
		const uint8_t *at = m->data + d->len;
		size_t left = (size_t)len - d->len - 4;
		uint32_t nh;

		memcpy(&nh, at, 4);
		at += 4;
		if (nh >= left / 4 || left - (size_t)nh * 4 > f->cap || hints_room(jit, nh)) {
			jit->stat_jc_misses++;
			return 0;
		}
		memcpy(jit->hints, at, (size_t)nh * 4);
		jit->nhints = nh;
		at += (size_t)nh * 4;
		left -= (size_t)nh * 4;
		f->len = left;
		f->overflow = 0;
		memcpy(f->data, at, f->len);
	}
	jit->stat_jc_hits++;
	return 1;
}

static void hint_keep(struct rv_cpu *cpu, const struct jit_region *r);

/* Keep the function just made (jit->func) in the store, after its description (made again in jit->body, which is free now). */
static void cache_keep(struct rv_cpu *cpu, const struct jit_region *r, const uint8_t key[RV_JC_KEY])
{
	struct rv_jit *jit = cpu->jit;
	struct wbuf *d = &jit->body;

	if (cache_describe(cpu, r, d))
		return;
	w_bytes(d, &jit->nhints, 4);
	w_bytes(d, jit->hints, 4 * (size_t)jit->nhints);
	w_bytes(d, jit->func.data, jit->func.len);
	if (!d->overflow && !rv_jc_put(key, d->data, d->len))
		jit->stat_jc_puts++;
	if (!jit->nhint)
		hint_keep(cpu, r);
}

/*
 * Is the code at pc a file's (a program's or a library's, mapped from its
 * file)? Only such code goes into the store: code a program makes as it runs
 * (a JavaScript engine's) is made anew, elsewhere, every time, and would only
 * fill it.
 */
static int file_code(struct rv_cpu *cpu, uint64_t pc)
{
	uint64_t dev, ino, offset, page = pc >> RV_PAGE_SHIFT;
	int fixed;

	if (page < cpu->npages && (cpu->map[page] & RV_PAGE_SHARED))
		return 1;
	return rv_mm_file_at(cpu, pc, &dev, &ino, &offset, &fixed);
}

/* ... all of region r (its first and its last block, at least). */
static int cache_file_code(struct rv_cpu *cpu, const struct jit_region *r)
{
	return file_code(cpu, r->blocks[0].pc) && file_code(cpu, r->blocks[r->nblocks - 1].pc);
}

/*
 * Programs start cold: every block is interpreted until it has run JIT_HOT
 * times, although the same program, run before, found out which blocks those
 * are. So the store also has, for every region kept there, a hint: the
 * blocks the region was made of, under a key made from where it begins and
 * the first bytes of code there (hint_key()). A block that runs for the
 * JIT_HINT_AT-th time and has a hint is made into its region at once, of the
 * blocks the hint names, which, if the code is the same, is the same region,
 * whose function the store then has as well (hint_translate()). A hint that
 * is out of date costs nothing but a region made of other blocks: the region
 * is made from the code that is there, whatever the hint says.
 */
#define JIT_HINT_AT	2
struct hint_head {
	uint32_t magic;
	uint32_t off;		/* of RV_JIT_OFF, what changes which blocks a region has */
	uint64_t pc;
	uint32_t nbytes;
	uint8_t bytes[20];
};
#define HINT_MAGIC	0x31686a72	/* "rjh1" */
#define HINT_BYTES	16

static int hint_key(struct rv_cpu *cpu, uint64_t pc, uint8_t key[RV_JC_KEY])
{
	struct hint_head h;
	uint64_t left = RV_PAGE_SIZE - (pc & (RV_PAGE_SIZE - 1));

	if (!code_page_now(cpu, pc))
		return -1;
	memset(&h, 0, sizeof(h));
	h.magic = HINT_MAGIC;
	h.off = cpu->jit->off & (OFF_WARM | OFF_RETURNS);
	h.pc = pc;
	h.nbytes = left < HINT_BYTES ? (uint32_t)left : HINT_BYTES;
	memcpy(h.bytes, rv_host_addr(cpu, pc), h.nbytes);
	rv_jc_key(&h, sizeof(h), key);
	return 0;
}

static int compare_pc(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* The hint for region r (just kept in the store): its blocks, sorted. */
static void hint_keep(struct rv_cpu *cpu, const struct jit_region *r)
{
	uint64_t pcs[JIT_MAX_BLOCKS];
	uint8_t key[RV_JC_KEY];

	if (hint_key(cpu, r->blocks[0].pc, key))
		return;
	for (int k = 0; k < r->nblocks; k++)
		pcs[k] = r->blocks[k].pc;
	qsort(pcs, (size_t)r->nblocks, sizeof(pcs[0]), compare_pc);
	rv_jc_put(key, pcs, (size_t)r->nblocks * sizeof(pcs[0]));
}

/* The block at pc has run JIT_HINT_AT times: made into a region at once if a hint says one began there. As translate(); -1 if there was no hint. */
static int hint_translate(struct rv_cpu *cpu, uint64_t pc)
{
	static __thread uint64_t pcs[JIT_MAX_BLOCKS];
	struct rv_jit *jit = cpu->jit;
	uint8_t key[RV_JC_KEY];
	long len;
	int done;

	jit->stat_hint_asked++;
	if (!file_code(cpu, pc) || hint_key(cpu, pc, key))
		return -1;
	len = rv_jc_get(key, pcs, sizeof(pcs));
	if (len <= 0 || (size_t)len > sizeof(pcs) || len % sizeof(pcs[0]))
		return -1;
	jit->stat_hint_found++;
	jit->hint = pcs;
	jit->nhint = (uint32_t)(len / sizeof(pcs[0]));
	done = translate(cpu, pc);
	jit->hint = NULL;
	jit->nhint = 0;
	if (done == 1 || done == 2)
		jit->stat_hint_made++;
	return done;
}

static int translate_region(struct rv_cpu *cpu, uint64_t pc)
{
	struct rv_jit *jit = cpu->jit;
	struct jit_region *r = jit->region;
	int timed = cpu->verbose || jit->log_fd >= 0;
	uint64_t t0 = timed ? now_ns() : 0, t1 = 0, t2 = 0;
	uint8_t key[RV_JC_KEY];
	int same, cached = 0, jc_file = 0;
	long slot = -1;
	uint32_t n;

#ifdef RV_HEAPCHECK
	rv_hc_light("before a translation");
#endif
	build_region(cpu, r, pc);
	if (!r->nblocks)
		return 0;
	if (timed)
		jit->lap[0] = now_ns() - t0;

	/*
	 * The map at most half full. Beyond that: if a good part of it is
	 * regions that are suspended or gone, those go and the rest stays
	 * (map_tidy()); else into a larger map, with everything in it; and if
	 * it is as large as it gets, the regions not entered lately go (as for
	 * room, evict()), and only if that is not enough, everything.
	 */
	if (jit->map_used + (uint32_t)r->nblocks > (1u << jit->map_bits) / 2) {
		if (jit->map_dead || jit->stat_suspended)
			map_tidy(jit);
		if (jit->map_used + (uint32_t)r->nblocks > (1u << jit->map_bits) * 3 / 8 && !map_grow(jit)) {
			if (jit->old & OLD_GROW_FLUSH)
				flush(jit);
		} else if (jit->map_used + (uint32_t)r->nblocks > (1u << jit->map_bits) * 3 / 8) {
			if (jit->map_bits + 2 <= MAP_BITS_MAX)
				jit->stat_nomem++;
			evict(jit, 0, 0);
			map_tidy(jit);
			if (jit->map_used + (uint32_t)r->nblocks > (1u << jit->map_bits) * 3 / 8)
				flush(jit);
		}
	}

	/* Room in the engine for its code (see "Room for translated code"). */
	if (!room_for(jit, now_ns())) {
		jit->stat_refused++;
		return 3;
	}

	/* A number for it: one that is free, or a new one. */
	if (regions_room(jit)) {
		jit->stat_nomem++;
		return 0;
	}
	n = jit->nfree ? jit->free_regions[jit->nfree - 1] : jit->nslots;
	/* The shape of the code, and the blocks numbered in its order (see struct layout). */
	layout_region(cpu, r, jit->layout);
	layout_renumber(r, jit->layout);
	if (target_table(jit, n, r)) {
		jit->stat_nomem++;
		return 0;
	}
	jit->making = n;
	if (jit->jc && !(jc_file = cache_file_code(cpu, r)))
		jit->stat_jc_other++;
	if (timed)
		t1 = now_ns();
	if (jc_file)
		cached = cache_find(cpu, r, key);
	if (timed) {
		t2 = now_ns();
		jit->lap[1] = t2 - t1;
	}
	if (cached && jit->jc_verify) {
		/* RV_JIT_CACHE_VERIFY: the function from the store must be the one made now, byte for byte. */
		size_t len = jit->func.len, at = 0;
		uint32_t nh = jit->nhints;
		uint8_t *stored = malloc((len ? len : 1) + 4 * (size_t)nh);

		if (stored) {
			memcpy(stored, jit->func.data, len);
			if (nh)
				memcpy(stored + len, jit->hints, 4 * (size_t)nh);
			gen_func(cpu, jit);
			if (jit->module.overflow) {
				free(stored);
				return -1;
			}
			while (at < len && at < jit->func.len && stored[at] == jit->func.data[at])
				at++;
			if (at < len || len != jit->func.len)
				fprintf(stderr, "rv: jit: store: the function for %#llx (%u blocks) is not the one made now: %zu bytes, now %zu, "
				       "first difference at %zu\n", (unsigned long long)r->blocks[0].pc, (unsigned)r->nblocks, len,
				       jit->func.len, at);
			else if (nh != jit->nhints || (nh && memcmp(stored + len, jit->hints, 4 * (size_t)nh)))
				fprintf(stderr, "rv: jit: store: the function for %#llx (%u blocks) is not the one made now: its branch hints "
				       "differ (%u, now %u)\n", (unsigned long long)r->blocks[0].pc, (unsigned)r->nblocks, nh, jit->nhints);
			free(stored);
			cached = 0;
		}
	}
	if (!cached) {
		gen_func(cpu, jit);
		if (jit->module.overflow)
			return -1;	/* (the caller has larger buffers made) */
		if (jc_file)
			cache_keep(cpu, r, key);
	}
	if (timed) {
		t1 = now_ns();
		jit->lap[2] = t1 - t2;
	}
	gen_wrap(cpu, jit);
	if (jit->module.overflow)
		return -1;
	same = module_seen(&jit->module);
	jit->stat_same += same;
	if (timed)
		jit->lap[3] = now_ns() - t1;

	if (n < jit->nslots)
		slot = jit->slots[n];
	t1 = timed ? now_ns() : 0;
	slot = install(jit, pc, slot);
	t2 = timed ? now_ns() : 0;
	jit->stat_ns_translate += t1 - t0;
	jit->stat_ns_install += t2 - t1;
	if (jit->log_fd >= 0)
		log_region(cpu, r, t1 - t0, t2 - t1, jit->module.len, same, cached);
	if (slot < 0) {
		jit->stat_failed++;
		return 0;
	}
	if (n == jit->nslots) {
		/* A new number: the slot is its own from now on, and says which number it is. */
		uint32_t at;

		if (!jit->nslots)
			jit->slot_base = (uint32_t)slot;
		at = (uint32_t)slot - jit->slot_base;
		if ((uint32_t)slot < jit->slot_base || at > jit->nslots + 0x100000) {
			jit->stat_failed++;	/* (not how the host gives out slots) */
			return 0;
		}
		if (at >= jit->by_slot_cap) {
			uint32_t cap = at + 256 + at / 2;
			uint32_t *by_slot = realloc(jit->by_slot, cap * sizeof(*by_slot));

			if (!by_slot) {
				jit->stat_nomem++;
				jit->stat_failed++;
				return 0;
			}
			memset(by_slot + jit->by_slot_cap, 0, (cap - jit->by_slot_cap) * sizeof(*by_slot));
			jit->by_slot = by_slot;
			jit->by_slot_cap = cap;
		}
		jit->by_slot[at] = n + 1;
		jit->slots[n] = (uint32_t)slot;
		jit->nslots++;
		jit->free_regions[jit->nfree++] = n;
	}
	/* (The number is the last of the free ones, either way.) */
	if (region_keep(jit, n, r)) {
		jit->stat_nomem++;
		jit->stat_failed++;
		return 0;
	}
	jit->nfree--;
	jit->made[n].kib = code_kib(jit->module.len);
	jit->code_kib += jit->made[n].kib;
	jit->used[n] = 1;
	/* (Its branch hints after it, in the same allocation: gen_module() left them as offsets in the function.) */
	if (!jit->batch_off && (jit->made[n].body = malloc(((jit->func.len + 3) & ~(size_t)3) + 4 * (size_t)jit->nhints))) {
		memcpy(jit->made[n].body, jit->func.data, jit->func.len);
		jit->made[n].body_len = (uint32_t)jit->func.len;
		jit->made[n].hints = (uint32_t *)(void *)(jit->made[n].body + ((jit->func.len + 3) & ~(size_t)3));
		jit->made[n].nhints = jit->nhints;
		if (jit->nhints)
			memcpy(jit->made[n].hints, jit->hints, 4 * (size_t)jit->nhints);
		jit->made[n].body_hash = body_hash(jit->func.data, jit->func.len);
		jit->made[n].bucket = batch_bucket(cpu, jit, pc);
		jit->pending[jit->npending++] = n;
	}

	for (int k = 0; k < r->nblocks; k++)
		map_insert(jit, r->blocks[k].pc, (uint32_t)slot, (uint32_t)k, r->blocks[k].count);
	pages_note(jit, n);
	if (jit->made[n].body)
		batch_trigger(cpu, jit, n);

#ifdef RV_HEAPCHECK
	rv_hc_light("after a translation");
#endif
	jit->stat_regions++;
	jit->stat_blocks += (uint64_t)r->nblocks;
	jit->stat_insns += (uint64_t)r->ninsns;
	jit->stat_bytes += jit->module.len;
	return 1;
}

/*
 * Is the code at pc the same as what region n was made from? 0 if all of
 * it is. 1 if a page of it is not code as things stand (not mapped, not to
 * be executed): the region may be good again later. 2 if it is other code
 * now. The pages are marked before they are read, as when translating.
 */
static int region_compare(struct rv_cpu *cpu, const struct jit_made *m)
{
	const uint8_t *was = m->bytes;
	int ret = 0;

	for (uint32_t k = 0; k < m->nblocks; k++) {
		uint64_t pc = m->pcs[k], left = m->sizes[k];

		while (left) {
			uint64_t part = RV_PAGE_SIZE - (pc & (RV_PAGE_SIZE - 1));

			if (part > left)
				part = left;
			if (!code_page(cpu, pc))
				ret = 1;
			else if (memcmp(rv_host_addr(cpu, pc), was, part))
				return 2;
			pc += part;
			was += part;
			left -= part;
		}
	}
	return ret;
}

/*
 * Execution has come to a block of a suspended region: if the code is still
 * what the region was made from, the region is in use again, and its entry
 * is returned. NULL if not: the interpreter runs the block.
 */
static struct jit_entry *revive(struct rv_cpu *cpu, struct jit_entry *e)
{
	struct rv_jit *jit = cpu->jit;
	uint64_t pc = e->pc & ~1ULL;
	struct jit_made *m;
	uint32_t n;

	if (e->slot == NO_SLOT)
		return NULL;
	n = region_of(jit, e->slot);
	m = &jit->made[n];
	switch (region_compare(cpu, m)) {
	case 1:
		return NULL;
	case 2:
		/* Other code now: that is translated when it has run often enough, and more often than this was. */
		jit->stat_changed++;
		region_cool(jit, n, 0);
		region_drop(jit, n);
		return NULL;
	}
	/*
	 * The same code. But a region that is suspended again and again when
	 * it has hardly run is made from a page that is written to all the
	 * time: the price of that is paid for every write, and interpreting
	 * the code is cheaper.
	 */
	if (m->revived && cpu->insn_count - m->revived < R_SOON) {
		if (++m->strikes >= R_STRIKES) {
			jit->stat_thrashed++;
			region_cool(jit, n, FAILS_THRASH);
			region_drop(jit, n);
			return NULL;
		}
	} else {
		m->strikes = 0;
	}
	m->revived = cpu->insn_count ? cpu->insn_count : 1;
	for (uint32_t k = 0; k < m->nblocks; k++) {
		struct jit_entry *other = map_find(jit, m->pcs[k]);

		/* (Not a block that a region made since has, and is in use with.) */
		if (other && !(other->pc & 1) && other->slot != e->slot)
			continue;
		map_insert(jit, m->pcs[k], jit->slots[n], k, m->counts[k]);
	}
	m->state = R_LIVE;
	jit->stat_revived++;
	return map_find(jit, pc);
}

/* Count an interpreted run of the block at pc; translate when it is hot. */
static int note_and_maybe_translate(struct rv_cpu *cpu, uint64_t pc)
{
	struct rv_jit *jit = cpu->jit;
	struct jit_count *c = &jit->counts[hash_pc(pc, jit->map_bits)];
	uint64_t need = jit->hot;
	int done;

	if (c->pc != pc) {
		c->pc = pc;
		c->count = 0;
		c->fails = 0;
		c->kind = 0;
	}
	if (c->count == NEVER)
		return 0;
	if (++c->count == JIT_HINT_AT && jit->jc && !jit->hints_off && jit->stat_regions < jit->limit) {
		switch (hint_translate(cpu, pc)) {
		case 1:
			return 1;
		case 2:
			return 0;
		}
		c = &jit->counts[hash_pc(pc, jit->map_bits)];	/* (translating may have replaced the table) */
		if (c->pc != pc)
			return 0;
	}
	if (c->count < jit->hot_made)
		return 0;
	if (jit->hot_made < jit->hot) {
		/*
		 * Code of a file's, or code the program made? Asked once, when it
		 * matters; and a page that is in a file's shared segment says
		 * so itself.
		 */
		if (!c->kind) {
			uint64_t dev, ino, offset, page = pc >> RV_PAGE_SHIFT;
			int fixed;

			if (page < cpu->npages && (cpu->map[page] & RV_PAGE_SHARED))
				c->kind = KIND_FILE;
			else
				c->kind = rv_mm_file_at(cpu, pc, &dev, &ino, &offset, &fixed) ? KIND_FILE : KIND_MADE;
		}
		if (c->kind == KIND_MADE)
			need = jit->hot_made;
	}
	if (c->count < need << c->fails || jit->stat_regions >= jit->limit)
		return 0;
	done = translate(cpu, pc);
	if (done == 1)
		return 1;
	if (done == 2)
		return 0;
	/*
	 * Looked up again: translating may have replaced the table (map_grow()
	 * frees the old one), whether it made the region or not. (Writing to
	 * the old one was writing to freed memory: the next free() of whatever
	 * had it by then aborted, in batch_make() in the browser.)
	 */
	c = &jit->counts[hash_pc(pc, jit->map_bits)];
	if (done == 3) {
		/* No room now: counted again from 0, and then asked again. */
		if (c->pc == pc)
			c->count = 0;
		return 0;
	}
	c->pc = pc;
	c->count = NEVER;
	return 0;
}

/*
 * RV_JIT_FAKE and RV_JIT_CHECK: the block that entry e stands for is about
 * to be run as translated. Is the code there still what it was translated
 * from? If not, the rules at the top have a hole: say where, and stop. (Only
 * while the guest has one thread: with more, one may be changing the code
 * while this one runs it, and until it flushes that is its own affair.)
 */
static void stale_check(struct rv_cpu *cpu, struct rv_jit *jit, const struct jit_entry *e)
{
	const struct jit_made *m = &jit->made[region_of(jit, e->slot)];
	const uint8_t *was = m->bytes;
	uint64_t pc = e->pc, left;

	if (cpu->proc->nthreads != 1 || m->state != R_LIVE || e->index >= m->nblocks || m->pcs[e->index] != pc)
		return;
	for (uint32_t k = 0; k < e->index; k++)
		was += m->sizes[k];
	for (left = m->sizes[e->index]; left; ) {
		uint64_t part = RV_PAGE_SIZE - (pc & (RV_PAGE_SIZE - 1));

		if (part > left)
			part = left;
		if (!code_page_now(cpu, pc) || memcmp(rv_host_addr(cpu, pc), was, part)) {
			fprintf(stderr, "rv: jit: a stale translation: the block at 0x%llx (block %u of the region in slot %u) "
				"is not the code it was translated from, at 0x%llx%s\n", (unsigned long long)e->pc, e->index,
				e->slot, (unsigned long long)pc, code_page_now(cpu, pc) ? "" : " (which is not code now)");
			_exit(98);
		}
		pc += part;
		was += part;
		left -= part;
	}
}

/*
 * The self-check (RV_JIT_CHECK=1). The interpreter is the reference: every
 * block is run by translated code and by the interpreter from the same
 * state, and both must arrive at the same registers, the same pc and the
 * same bytes in memory. To start both from the same state, what a run wrote
 * to memory is taken back afterwards (the interpreter logs its writes; the
 * translated code writes the same places or it is wrong anyway). The first
 * difference is reported and ends the program.
 *
 * One block at a time: regions are called with no budget, which lets them
 * run the block they are entered at and nothing more, and chaining is off.
 *
 * Only while the guest has one thread: with more, memory may differ between
 * the two runs because another thread wrote to it, and that is no fault.
 */
#define UNDO_MAX	(JIT_BLOCK_INSNS + 8)

static __thread struct {
	uint64_t addr, len;
	uint8_t old[16];
} undo[UNDO_MAX];
static __thread int undo_count;

void rv_undo_note(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	uint64_t in_page = RV_PAGE_SIZE - (addr & (RV_PAGE_SIZE - 1));

	/* (An access across two pages: each part where its page is.) */
	if (len > in_page) {
		rv_undo_note(cpu, addr, in_page);
		addr += in_page;
		len -= in_page;
	}
	if (undo_count < UNDO_MAX && len <= sizeof(undo[0].old)) {
		undo[undo_count].addr = addr;
		undo[undo_count].len = len;
		memcpy(undo[undo_count].old, rv_host_addr(cpu, addr), len);
		undo_count++;
	}
}

static void checked_call(struct rv_cpu *cpu, struct rv_jit *jit, const struct jit_entry *e)
{
	static __thread struct rv_cpu before, got;
	static __thread uint8_t got_bytes[UNDO_MAX][16];
	uint32_t len = jit->map_len[e - jit->map];
	uint64_t ran;
	int bad = 0, where = -1;

	stale_check(cpu, jit, e);

	/* A first run by the interpreter, only to learn which memory the block writes; then everything back. */
	before = *cpu;
	undo_count = 0;
	cpu->undo_on = 1;
	rv_interp_steps(cpu, len);
	cpu->undo_on = 0;
	for (int i = undo_count - 1; i >= 0; i--)
		memcpy(rv_host_addr(cpu, undo[i].addr), undo[i].old, undo[i].len);
	*cpu = before;

	/*
	 * That run made code change (the block writes to a page code was
	 * translated from, its own perhaps, or made the page table grow): the
	 * translated block would have left for the interpreter at that
	 * instruction, and no longer does now that the page may be written.
	 * Nothing to compare: the interpreter runs the block, and the
	 * dispatcher looks at what changed.
	 */
	if (jit->epoch != cpu->proc->code_epoch) {
		rv_interp_block(cpu);
		jit->stat_unchecked++;
		return;
	}

	/* The translated block: keep what it did, and take that back too. */
	region_call(jit, e->slot, cpu, e->index, 0);
	got = *cpu;
	ran = cpu->insn_count - before.insn_count;
	/* (All of it read before any of it is put back: a block may write one place twice.) */
	for (int i = 0; i < undo_count; i++)
		memcpy(got_bytes[i], rv_host_addr(cpu, undo[i].addr), undo[i].len);
	for (int i = undo_count - 1; i >= 0; i--)
		memcpy(rv_host_addr(cpu, undo[i].addr), undo[i].old, undo[i].len);
	*cpu = before;

	/*
	 * The reference: as many instructions as the translated code ran. That
	 * is the whole block, or fewer if it left an instruction to the
	 * interpreter; none at all means the block's first instruction, which
	 * the interpreter now executes (as the dispatcher has it do).
	 */
	if (!ran) {
		rv_interp_block(cpu);
		jit->stat_unchecked++;
		return;
	}
	rv_interp_steps(cpu, ran);
	if (cpu->trap != RV_TRAP_NONE) {
		jit->stat_unchecked++;
		return;
	}
	jit->stat_checked++;

	if (cpu->pc != got.pc || cpu->fcsr != got.fcsr)
		bad = 1;
	for (int r = 0; r < 32; r++)
		if (cpu->x[r] != got.x[r] || cpu->f[r] != got.f[r])
			bad = 1;
	for (int i = 0; i < undo_count; i++) {
		if (memcmp(rv_host_addr(cpu, undo[i].addr), got_bytes[i], undo[i].len)) {
			bad = 1;
			where = i;
		}
	}
	if (!bad)
		return;

	fprintf(stderr, "rv: jit check: the block at 0x%llx (%llu of its %u instructions run, block %u of the region "
		"in slot %u) went wrong after %llu good blocks\n", (unsigned long long)before.pc,
		(unsigned long long)ran, len, e->index, e->slot, (unsigned long long)jit->stat_checked - 1);
	if (cpu->pc != got.pc)
		fprintf(stderr, "  pc: interpreter 0x%llx, translated 0x%llx\n",
			(unsigned long long)cpu->pc, (unsigned long long)got.pc);
	if (cpu->fcsr != got.fcsr)
		fprintf(stderr, "  fcsr: interpreter 0x%x, translated 0x%x\n", cpu->fcsr, got.fcsr);
	for (int r = 0; r < 32; r++) {
		if (cpu->x[r] != got.x[r])
			fprintf(stderr, "  x%d: before 0x%llx, interpreter 0x%llx, translated 0x%llx\n", r,
				(unsigned long long)before.x[r], (unsigned long long)cpu->x[r],
				(unsigned long long)got.x[r]);
		if (cpu->f[r] != got.f[r])
			fprintf(stderr, "  f%d: before 0x%llx, interpreter 0x%llx, translated 0x%llx\n", r,
				(unsigned long long)before.f[r], (unsigned long long)cpu->f[r],
				(unsigned long long)got.f[r]);
	}
	if (where >= 0) {
		uint64_t want_bytes = 0, bytes = 0;
		uint64_t n = undo[where].len < 8 ? undo[where].len : 8;

		memcpy(&want_bytes, rv_host_addr(cpu, undo[where].addr), n);
		memcpy(&bytes, got_bytes[where], n);
		fprintf(stderr, "  memory at 0x%llx (%llu bytes): interpreter 0x%llx, translated 0x%llx\n",
			(unsigned long long)undo[where].addr, (unsigned long long)undo[where].len,
			(unsigned long long)want_bytes, (unsigned long long)bytes);
	}
	fprintf(stderr, "  the block:");
	{
		struct jit_insn insn;
		uint64_t pc = before.pc;

		for (uint32_t k = 0; k < len && fetch(cpu, pc, &insn) != K_NONE; k++, pc += insn.len)
			fprintf(stderr, " %llx:%08x", (unsigned long long)pc, insn.raw);
	}
	fprintf(stderr, "\n  registers before:");
	for (int r = 1; r < 32; r++)
		fprintf(stderr, " x%d=0x%llx", r, (unsigned long long)before.x[r]);
	fprintf(stderr, "\n");
	_exit(99);
}

/*
 * RV_JIT_SNAP=DIR: the guest's whole state as translated code is about to be
 * entered, written once per region (its first entry; at most RV_JIT_SNAP_MAX
 * of them, 64 by default), for running the dumped regions on a real engine
 * from the same state (tools/region-bench.js). The file: a header, the
 * registers, then every page that has memory, with its flags.
 */
struct snap_head {
	char magic[8];
	uint64_t pc, insn_count, region_pc;
	uint32_t entry, slot, fcsr, lr_valid;
	uint64_t lr_addr, lr_value;
	uint64_t x[32], f[32];
	uint64_t npages, nmapped;
};

static void snapshot(struct rv_cpu *cpu, struct rv_jit *jit, const struct jit_entry *e)
{
	uint32_t n = region_of(jit, e->slot);
	struct snap_head h;
	char path[512];
	FILE *out;

	if (jit->nsnaps >= jit->snap_max)
		return;
	if (jit->snap_every) {
		if (cpu->insn_count < jit->snap_next)
			return;
		jit->snap_next = cpu->insn_count + jit->snap_every;
	} else {
		if (jit->made[n].snapped)
			return;
		jit->made[n].snapped = 1;
	}
	snprintf(path, sizeof(path), "%s/snap-%u-%llx.bin", jit->snap_dir, jit->nsnaps++, (unsigned long long)cpu->pc);
	if (!(out = fopen(path, "wb")))
		return;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "RVSNAP01", 8);
	h.pc = cpu->pc;
	h.insn_count = cpu->insn_count;
	h.region_pc = jit->made[n].pcs[0];
	h.entry = e->index;
	h.slot = e->slot;
	h.fcsr = cpu->fcsr;
	h.lr_valid = (uint32_t)cpu->lr_valid;
	h.lr_addr = cpu->lr_addr;
	h.lr_value = cpu->lr_value;
	memcpy(h.x, cpu->x, sizeof(h.x));
	memcpy(h.f, cpu->f, sizeof(h.f));
	h.npages = cpu->npages;
	for (uint64_t p = 0; p < cpu->npages; p++)
		h.nmapped += (cpu->map[p] & RV_PAGE_BACKED) != 0;
	fwrite(&h, sizeof(h), 1, out);
	for (uint64_t p = 0; p < cpu->npages; p++) {
		uint64_t flags = cpu->map[p] & RV_PAGE_FLAGS;

		if (!(cpu->map[p] & RV_PAGE_BACKED))
			continue;
		fwrite(&p, 8, 1, out);
		fwrite(&flags, 8, 1, out);
		fwrite(rv_host_addr(cpu, p << RV_PAGE_SHIFT), RV_PAGE_SIZE, 1, out);
	}
	fclose(out);
}

/* The offsets translated code was made with, for the same tool: once per directory. */
static void snapshot_layout(const char *dir)
{
	char path[512];
	FILE *out;

	snprintf(path, sizeof(path), "%s/layout.json", dir);
	if (!(out = fopen(path, "w")))
		return;
	fprintf(out, "{\n");
#define OFF(name, field) fprintf(out, "  \"%s\": %zu,\n", name, offsetof(struct rv_cpu, field))
	OFF("x", x); OFF("f", f); OFF("pc", pc); OFF("fcsr", fcsr); OFF("lr_addr", lr_addr); OFF("lr_value", lr_value);
	OFF("lr_valid", lr_valid); OFF("proc", proc); OFF("map", map); OFF("npages", npages); OFF("reserve_seen", reserve_seen);
	OFF("jit", jit); OFF("jit_used", jit_used); OFF("jit_fault", jit_fault); OFF("sig_pending", sig_pending);
	OFF("insn_count", insn_count);
#undef OFF
	fprintf(out, "  \"sizeof_cpu\": %zu,\n", sizeof(struct rv_cpu));
	fprintf(out, "  \"proc_reserve_gen\": %zu,\n", offsetof(struct rv_proc, reserve_gen));
	fprintf(out, "  \"jit_map\": %zu,\n  \"jit_map_bits\": %zu,\n  \"jit_stat_chained\": %zu,\n  \"jit_stat_misses\": %zu,\n",
		offsetof(struct rv_jit, map), offsetof(struct rv_jit, map_bits), offsetof(struct rv_jit, stat_chained),
		offsetof(struct rv_jit, stat_misses));
	fprintf(out, "  \"entry_size\": %zu,\n  \"entry_pc\": %zu,\n  \"entry_slot\": %zu,\n  \"entry_index\": %zu,\n",
		sizeof(struct jit_entry), offsetof(struct jit_entry, pc), offsetof(struct jit_entry, slot), offsetof(struct jit_entry, index));
	fprintf(out, "  \"map_probes\": %d,\n  \"hash_multiplier\": \"%llx\",\n  \"used_bits\": %d,\n  \"budget\": %d\n}\n",
		MAP_PROBES, (unsigned long long)HASH_MULTIPLIER, USED_BITS, JIT_BUDGET);
	fclose(out);
}

/* The dispatcher: translated code where there is some, the interpreter elsewhere. */
void rv_jit_run(struct rv_cpu *cpu)
{
	struct rv_jit *jit = cpu->jit;

	while (cpu->trap == RV_TRAP_NONE) {
		struct jit_entry *e;

		/* Signals, and parking when asked to (thread.c): the arena may be elsewhere afterwards. */
		if (__builtin_expect(cpu->sig_pending || cpu->sig_restore_mask, 0)) {
			rv_signal_deliver(cpu);
			if (cpu->trap != RV_TRAP_NONE)
				break;
		}
		/* Code changed, or the page table moved: what was translated from that is not to be run as it is. */
		if (__builtin_expect(jit->epoch != cpu->proc->code_epoch, 0))
			catch_up(cpu);
		if (cpu->insn_count - jit->last_poll >= JIT_POLL) {
			rv_signal_poll();
			jit->last_poll = cpu->insn_count;
			if (cpu->insn_count - jit->last_look >= ROOM_LOOK)
				room_look(cpu, jit);
		}

		e = map_find(jit, cpu->pc);
		if (e && __builtin_expect(e->pc & 1, 0))
			e = revive(cpu, e);	/* a block of a suspended region */
		else if (!e && note_and_maybe_translate(cpu, cpu->pc))
			e = map_find(jit, cpu->pc);
		if (e && __builtin_expect(jit->snap_dir != NULL, 0))
			snapshot(cpu, jit, e);
		if (e && jit->fake) {
			jit->used[region_of(jit, e->slot)] = 1;
			stale_check(cpu, jit, e);
			rv_interp_block(cpu);
			jit->stat_calls++;
		} else if (e && jit->check && cpu->proc->nthreads == 1) {
			checked_call(cpu, jit, e);
		} else if (e) {
			uint64_t before = cpu->insn_count;

			region_call(jit, e->slot, cpu, e->index, JIT_BUDGET);
			jit->stat_ran += cpu->insn_count - before;
			jit->stat_calls++;
			/*
			 * Nothing ran: the first instruction of the block is one the
			 * translated code leaves to the interpreter this time (an
			 * access across a page boundary, or a fault). pc is still the
			 * start of the block, so looking it up again would only enter
			 * the block again: the interpreter has to take a step first.
			 */
			if (cpu->insn_count == before && !cpu->sig_pending) {
				rv_interp_block(cpu);
				continue;
			}
			if (cpu->verbose) {
				struct jit_entry *to = map_find(jit, cpu->pc);

				if (cpu->insn_count - before >= JIT_BUDGET)
					jit->stat_budget++;
				else if (!to) {
					struct jit_insn insn;

					jit->stat_to_interp++;
					/* Opcode 0 stands for "could be translated": a block that is not hot yet. */
					if (fetch_as(cpu, cpu->pc, &insn, 0) == K_NONE && code_page_now(cpu, cpu->pc))
						jit->stat_opcode[OPCODE(insn.raw)]++;
					else
						jit->stat_opcode[0]++;
				}
				else if (to->slot != e->slot)
					jit->stat_to_other++;
				else if (to != &jit->map[hash_pc(cpu->pc, jit->map_bits)])
					jit->stat_to_self_displaced++;
				else
					jit->stat_to_self++;
			}
		} else {
			rv_interp_block(cpu);
		}
	}
}

int rv_jit_init(struct rv_cpu *cpu)
{
	const char *hot = getenv("RV_JIT_HOT"), *hot_made = getenv("RV_JIT_HOT_MADE");
	struct rv_jit *jit = calloc(1, sizeof(*jit));

	if (!jit)
		return -1;
	jit->check = getenv("RV_JIT_CHECK") != NULL;
	if (!(jit->used_pc = calloc(1, USED_PLACES)) || map_alloc(jit, MAP_BITS_MIN)) {
		free(jit->used_pc);
		free(jit);
		return -1;
	}
	cpu->jit_used = jit->used_pc;
	jit->epoch = cpu->proc->code_epoch;
	jit->hot = hot && atoi(hot) > 0 ? (uint32_t)atoi(hot) : JIT_HOT;
	jit->hot_made = hot_made && atoi(hot_made) > 0 ? (uint32_t)atoi(hot_made) : JIT_HOT_MADE;
	if (jit->hot_made > jit->hot)
		jit->hot_made = jit->hot;
#ifndef __wasm__
	jit->emu = getenv("RV_JIT_EMU") != NULL;
	jit->fake = !jit->emu && getenv("RV_JIT_FAKE") != NULL;
	jit->fake_slot = 16;
#endif
	jit->warm = getenv("RV_JIT_WARM") ? (uint32_t)atoi(getenv("RV_JIT_WARM")) : JIT_WARM;
	jit->off = getenv("RV_JIT_OFF") ? (uint32_t)atoi(getenv("RV_JIT_OFF")) : 0;
	jit->hint_sites = getenv("RV_JIT_HINT_SITES") ? (uint32_t)strtoul(getenv("RV_JIT_HINT_SITES"), NULL, 0) & HS_ALL : HS_ALL;
	jit->tag = getenv("RV_JIT_TAG") != NULL;
	jit->stat_ns_start = now_ns();
	if (jit->tag)
		pthread_once(&tag_once, tag_init);
	jit->limit = getenv("RV_JIT_LIMIT") ? strtoull(getenv("RV_JIT_LIMIT"), NULL, 0) : ~0ULL;
	if (jit->check)
		jit->off |= OFF_CHAIN;
	jit->dump_dir = getenv("RV_JIT_DUMP");
#ifndef __wasm__
	if (getenv("RV_JIT_FLATMEM") && jit->dump_dir) {
		jit->flatmem = strtoull(getenv("RV_JIT_FLATMEM"), NULL, 0);
		jit->flatmem_on = 1;
	}
#endif
	/*
	 * The defaults, from the measurements (see the report in the branch's
	 * description): the translation cache and the nesting of blocks as
	 * loops are both opt-in. The cache gains 20-50% on code that walks
	 * arrays and stacks, but loses as much where pointers are followed from
	 * object to object (every access a miss), and no static choice of
	 * registers told the two apart on every program; the nesting helps
	 * TurboFan a little and QuickJS's dispatch in Liftoff, but costs 9x in
	 * TurboFan compile time and 20% in Liftoff on Lua. RV_JIT_PCACHE=n (up
	 * to PCACHE_SLOTS registers) and RV_JIT_STRUCT=1 turn them on.
	 */
	jit->pcache_slots = getenv("RV_JIT_PCACHE") ? atoi(getenv("RV_JIT_PCACHE")) : 0;
	if (jit->pcache_slots > PCACHE_SLOTS)
		jit->pcache_slots = PCACHE_SLOTS;
	if (!(getenv("RV_JIT_STRUCT") && atoi(getenv("RV_JIT_STRUCT"))))
		jit->off |= OFF_STRUCT;
	/* The indirect-jump profile is opt-in too: it makes interpreter dispatch tables 2.4x faster
	 * (list) but costs Lua 26% and no shape of it is free; RV_JIT_JALRPROF=1 turns it on. */
	if (!(getenv("RV_JIT_JALRPROF") && atoi(getenv("RV_JIT_JALRPROF"))))
		jit->off |= OFF_JALRPROF;
	jit->snap_dir = getenv("RV_JIT_SNAP");
	jit->snap_max = getenv("RV_JIT_SNAP_MAX") ? (uint32_t)atoi(getenv("RV_JIT_SNAP_MAX")) : 64;
	jit->snap_every = getenv("RV_JIT_SNAP_EVERY") ? strtoull(getenv("RV_JIT_SNAP_EVERY"), NULL, 0) : 0;
	if (jit->dump_dir)
		snapshot_layout(jit->dump_dir);
	jit->names = getenv("RV_JIT_NAMES") && atoi(getenv("RV_JIT_NAMES"));
	jit->old = getenv("RV_JIT_OLD") ? (uint32_t)atoi(getenv("RV_JIT_OLD")) : 0;
	jit->batch_min = JIT_BATCH_MIN;
	jit->batch_mask = JIT_BATCH_MASK;
	jit->group_shift = JIT_GROUP_SHIFT;
	if (getenv("RV_JIT_GROUP")) {
		unsigned bmin, bmask, bshift;

		if (sscanf(getenv("RV_JIT_GROUP"), "%u:%u:%u", &bmin, &bmask, &bshift) == 3 && bmin >= 1 && bmin < JIT_BATCH_MAX) {
			jit->batch_min = bmin;
			jit->batch_mask = bmask;
			jit->group_shift = bshift;
		}
	}
	jit->batch_off = getenv("RV_JIT_BATCH") && !atoi(getenv("RV_JIT_BATCH"));
	jit->refuse = getenv("RV_JIT_REFUSE") ? (uint32_t)atoi(getenv("RV_JIT_REFUSE")) : 0;
	jit->code_cap_kib = (getenv("RV_JIT_CODE_MB") && atoi(getenv("RV_JIT_CODE_MB")) > 0 ?
			     (uint64_t)atoi(getenv("RV_JIT_CODE_MB")) : 512) * 1024;
	jit->jc = !jit->tag && rv_jc_on();
	jit->jc_verify = getenv("RV_JIT_CACHE_VERIFY") != NULL;
	jit->hints_off = getenv("RV_JIT_HINTS") && !atoi(getenv("RV_JIT_HINTS"));
	jit->log_fd = getenv("RV_JIT_LOG") ? open(getenv("RV_JIT_LOG"), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
	cpu->jit = jit;
	return 0;
}

/* A thread ends: its translations go with it (the table they were in was its instance's). */
void rv_jit_free(struct rv_cpu *cpu)
{
	struct rv_jit *jit = cpu->jit;

	if (!jit)
		return;
	for (uint32_t i = 0; jit->targets && i < jit->slots_cap; i++)
		free(jit->targets[i]);
	for (uint32_t i = 0; jit->made && i < jit->nslots; i++) {
		free(jit->made[i].pcs);
		free(jit->made[i].body);
	}
	free(jit->batch_live);
	free(jit->batch_size);
	free(jit->hints);
	free(jit->targets);
	free(jit->used);
	free(jit->used_pc);
	free(jit->made);
	free(jit->free_regions);
	free(jit->by_slot);
	free(jit->pages);
	free(jit->slots);
	free(jit->map);
	free(jit->map_len);
	free(jit->counts);
#ifndef __wasm__
	if (jit->emu)
		rv_emu_thread_exit();
#endif
	free(jit);
	cpu->jit = NULL;
	cpu->jit_used = NULL;
}

/*
 * With -v: a line for a thread, where a program of many threads spends its
 * time (Chromium has thirty and more per process): how much the thread ran,
 * how much of that translated. For each thread that ends having run a
 * million instructions or more, and at the end of the process for each one
 * still there (the thread that ends the process also reports in full,
 * rv_jit_report()).
 */
void rv_jit_report_thread(struct rv_cpu *t)
{
	const struct rv_jit *jit = t->jit;
	char path[64], name[32] = "?";
	FILE *f;

	if (t->insn_count < 1000000)
		return;
	snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)t->tid);
	if ((f = fopen(path, "r"))) {
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = '\0';
		fclose(f);
	}
	fprintf(stderr, "rv: thread %d (%s): %llu instructions, %llu%% of them translated; %llu regions, "
		"%llu ms making them, %llu ms compiling; left for the interpreter %llu times; "
		"%llu regions let go of for room, %llu put off; %llu regions made before in the process\n",
		(int)t->tid, name, (unsigned long long)t->insn_count,
		(unsigned long long)(jit ? jit->stat_ran * 100 / t->insn_count : 0),
		(unsigned long long)(jit ? jit->stat_regions : 0),
		(unsigned long long)(jit ? jit->stat_ns_translate / 1000000 : 0),
		(unsigned long long)(jit ? jit->stat_ns_install / 1000000 : 0),
		(unsigned long long)(jit ? jit->stat_to_interp : 0),
		(unsigned long long)(jit ? jit->stat_evicted : 0),
		(unsigned long long)(jit ? jit->stat_refused : 0), (unsigned long long)(jit ? jit->stat_same : 0));
}

void rv_jit_report_threads(struct rv_cpu *cpu)
{
	rv_lock(cpu);
	for (struct rv_cpu *t = cpu->proc->threads; t; t = t->next_thread)
		rv_jit_report_thread(t);
	rv_unlock(cpu);
}

void rv_jit_report(struct rv_cpu *cpu)
{
	struct rv_jit *jit = cpu->jit;

	if (!jit)
		return;
#ifndef __wasm__
	if (jit->emu)
		rv_emu_report();
#endif
	fprintf(stderr, "rv: jit: of %llu ms since this thread's translator began, %llu ms went into making regions and %llu ms into the host compiling them\n",
		(unsigned long long)((now_ns() - jit->stat_ns_start) / 1000000), (unsigned long long)(jit->stat_ns_translate / 1000000),
		(unsigned long long)(jit->stat_ns_install / 1000000));
	fprintf(stderr, "rv: jit: %llu regions (%llu blocks, %llu instructions, %llu bytes of wasm), "
		"%llu flushes, %llu failed, %llu times short of memory\n",
		(unsigned long long)jit->stat_regions, (unsigned long long)jit->stat_blocks,
		(unsigned long long)jit->stat_insns, (unsigned long long)jit->stat_bytes,
		(unsigned long long)jit->stat_flushes, (unsigned long long)jit->stat_failed,
		(unsigned long long)jit->stat_nomem);
	if (jit->stat_evictions || jit->stat_refused || cpu->verbose)
		fprintf(stderr, "rv: jit: room: this thread's modules take about %llu KiB (at most %llu); %llu regions "
			"(%llu KiB) let go of in %llu rounds, %llu translations put off for want of room%s\n",
			(unsigned long long)jit->code_kib, (unsigned long long)jit->code_cap_kib,
			(unsigned long long)jit->stat_evicted, (unsigned long long)jit->stat_evicted_kib,
			(unsigned long long)jit->stat_evictions, (unsigned long long)jit->stat_refused,
			jit->no_release ? " (the host cannot let go of a module)" : "");
	if (jit->stat_batches || jit->stat_batch_failed)
		fprintf(stderr, "rv: jit: %llu batches made of %llu regions, %llu failed\n",
			(unsigned long long)jit->stat_batches, (unsigned long long)jit->stat_batched,
			(unsigned long long)jit->stat_batch_failed);
	if (jit->jc)
		fprintf(stderr, "rv: jit: store of translations: %llu regions found there, %llu not (%llu kept), %llu not of a file; "
			"hints: %llu looked for, %llu found, %llu regions made from them\n",
			(unsigned long long)jit->stat_jc_hits, (unsigned long long)jit->stat_jc_misses,
			(unsigned long long)jit->stat_jc_puts, (unsigned long long)jit->stat_jc_other,
			(unsigned long long)jit->stat_hint_asked, (unsigned long long)jit->stat_hint_found,
			(unsigned long long)jit->stat_hint_made);
	if (jit->stat_events || jit->stat_suspended || jit->stat_tidied)
		fprintf(stderr, "rv: jit: code changed %llu times for this thread: %llu regions suspended, %llu taken up again "
			"as they were, %llu found changed, %llu given up as written to all the time; the map tidied %llu times\n",
			(unsigned long long)jit->stat_events, (unsigned long long)jit->stat_suspended,
			(unsigned long long)jit->stat_revived, (unsigned long long)jit->stat_changed,
			(unsigned long long)jit->stat_thrashed, (unsigned long long)jit->stat_tidied);
	{
		const uint64_t *why = cpu->proc->code_events;

		if (why[RV_CODE_MOVED] + why[RV_CODE_WRITE] + why[RV_CODE_PROTECT] + why[RV_CODE_UNMAP] + why[RV_CODE_FLUSH])
			fprintf(stderr, "rv: jit: pages that code was translated from, in all threads: written to %llu times, "
				"made not executable %llu, unmapped or replaced %llu, flushed by the program %llu; "
				"the page table moved %llu times\n",
				(unsigned long long)why[RV_CODE_WRITE], (unsigned long long)why[RV_CODE_PROTECT],
				(unsigned long long)why[RV_CODE_UNMAP], (unsigned long long)why[RV_CODE_FLUSH],
				(unsigned long long)why[RV_CODE_MOVED]);
	}
	if (cpu->insn_count)
		fprintf(stderr, "rv: jit: translated code ran %llu instructions (%llu%%) in %llu calls, "
			"with %llu jumps from region to region\n",
			(unsigned long long)jit->stat_ran,
			(unsigned long long)(jit->stat_ran * 100 / cpu->insn_count),
			(unsigned long long)jit->stat_calls, (unsigned long long)jit->stat_chained);
	if (jit->check)
		fprintf(stderr, "rv: jit check: %llu blocks agreed with the interpreter, %llu could not be compared\n",
			(unsigned long long)jit->stat_checked, (unsigned long long)jit->stat_unchecked);
	if (cpu->verbose && jit->stat_calls)
		fprintf(stderr, "rv: jit: regions were left for: another region %llu, the interpreter %llu, "
			"a block of the same region %llu (+%llu not in its first map slot), the budget %llu\n",
			(unsigned long long)jit->stat_to_other, (unsigned long long)jit->stat_to_interp,
			(unsigned long long)jit->stat_to_self, (unsigned long long)jit->stat_to_self_displaced,
			(unsigned long long)jit->stat_budget);
	for (int op = 0; cpu->verbose && op < 128; op++)
		if (jit->stat_opcode[op] * 100 >= jit->stat_calls && jit->stat_opcode[op])
			fprintf(stderr, "rv: jit:   interpreter needed at %s 0x%02x: %llu\n",
				op ? "opcode" : "translatable code, opcode", op,
				(unsigned long long)jit->stat_opcode[op]);
	if (cpu->verbose) {
		/* The kinds of rv_jit_count_fp(). */
		static const char *const names[32] = {
			"fadd", "fsub", "fmul", "fdiv", "fsgnj", "fmin/fmax", "?", "?", "fcvt.s.d/d.s", "?", "?", "fsqrt",
			"?", "?", "?", "?", "?", "?", "?", "?", "feq/flt/fle", "?", "?", "?", "fcvt to an integer", "?",
			"fcvt from an integer", "?", "fmv.x/fclass", "?", "fmv.w.x/fmv.d.x", "?",
		};
		static const char *const fused[4] = { "fmadd", "fmsub", "fnmsub", "fnmadd" };
		uint64_t total = 0;

		for (int k = 0; k < 72; k++)
			total += jit->stat_fp_call[k];
		if (total)
			fprintf(stderr, "rv: jit: floating point left to the call: %llu instructions\n", (unsigned long long)total);
		if (jit->fma)
			fprintf(stderr, "rv: jit: fused multiply-add by %s\n", jit->fma > 0 ? "the host's instruction" : "the emulator's own arithmetic");
		for (int k = 0; k < 72; k++)
			if (jit->stat_fp_call[k] && jit->stat_fp_call[k] * 1000 >= total)
				fprintf(stderr, "rv: jit:   %s%s: %llu\n", k < 64 ? names[k & 31] : fused[k & 3],
					(k & 32) ? ".s" : ".d", (unsigned long long)jit->stat_fp_call[k]);
	}
}

/*
 * With -v: translated code left a floating-point instruction to the call.
 * Counted by kind: the ordinary ones by the top five bits of the
 * instruction (their group), the fused ones from 64 on by the two bits of
 * their opcode that differ; +32, or +4, for singles.
 */
void rv_jit_count_fp(struct rv_cpu *cpu, uint32_t insn)
{
	struct rv_jit *jit = cpu->jit;
	uint32_t k, single = !((insn >> 25) & 1);

	if (!jit)
		return;
	if ((insn & 0x7f) == 0x53)
		k = (insn >> 27) + (single ? 32 : 0);
	else
		k = 64 + ((insn >> 2) & 3) + (single ? 4 : 0);
	jit->stat_fp_call[k % 72]++;
	/* RV_JIT_FPLOG=N: the state at the N-th such call and the 39 after it, to see why the call was needed. */
	{
		static long from = -2;
		uint64_t n = 0;

		if (from == -2)
			from = getenv("RV_JIT_FPLOG") ? atol(getenv("RV_JIT_FPLOG")) : -1;
		if (from < 0)
			return;
		for (int i = 0; i < 72; i++)
			n += jit->stat_fp_call[i];
		if (n >= (uint64_t)from && n < (uint64_t)from + 40)
			fprintf(stderr, "rv: fp call %llu: insn %08x fcsr %02x f%u=%016llx f%u=%016llx f%u=%016llx\n",
				(unsigned long long)n, insn, cpu->fcsr, (insn >> 15) & 31, (unsigned long long)cpu->f[(insn >> 15) & 31],
				(insn >> 20) & 31, (unsigned long long)cpu->f[(insn >> 20) & 31], insn >> 27,
				(unsigned long long)cpu->f[insn >> 27]);
	}
}
