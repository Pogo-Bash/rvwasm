/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Static ELF loader and initial stack for riscv64 guests.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rv.h"

#define EI_NIDENT	16
#define ET_EXEC		2
#define ET_DYN		3
#define EM_RISCV	243
#define PT_LOAD		1
#define PT_INTERP	3
#define PT_PHDR		6
#define PF_X		1
#define PF_W		2
#define PF_R		4

#define AT_NULL		0
#define AT_PHDR		3
#define AT_PHENT	4
#define AT_PHNUM	5
#define AT_PAGESZ	6
#define AT_BASE		7
#define AT_FLAGS	8
#define AT_ENTRY	9
#define AT_UID		11
#define AT_EUID		12
#define AT_GID		13
#define AT_EGID		14
#define AT_HWCAP	16
#define AT_CLKTCK	17
#define AT_SECURE	23
#define AT_RANDOM	25
#define AT_EXECFN	31

/* Where a position-independent executable is placed in the arena. */
#define RV_PIE_BASE	0x100000ULL

/* riscv AT_HWCAP: one bit per single-letter extension. */
#define HWCAP_ISA(c)	(1UL << ((c) - 'A'))
#define RV_HWCAP	(HWCAP_ISA('I') | HWCAP_ISA('M') | HWCAP_ISA('A') | \
			 HWCAP_ISA('F') | HWCAP_ISA('D') | HWCAP_ISA('C'))

struct elf64_ehdr {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = pread(fd, p, len, (off_t)off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

#define MAX_PHDRS	64

struct image {
	uint64_t entry;		/* where execution starts */
	uint64_t phdr;		/* guest address of the program headers */
	uint64_t phnum;
	uint64_t base;		/* load bias: 0 for a non-PIE executable */
	uint64_t end;		/* end of the highest segment */
	char interp[256];	/* PT_INTERP, or empty */
};

/*
 * Load one ELF image into the arena. A fixed-address executable goes where it
 * was linked. A position-independent one goes to RV_PIE_BASE if it is the
 * program itself, and into free space below the stack if it is the program's
 * interpreter (the dynamic loader).
 */
static int load_image(struct rv_cpu *cpu, const char *path, int is_interp, struct image *img)
{
	static struct elf64_phdr ph[MAX_PHDRS];
	struct elf64_ehdr eh;
	uint64_t lo = UINT64_MAX, hi = 0;
	int fd, ret = -1;

	memset(img, 0, sizeof(*img));
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "rv: cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (read_at(fd, &eh, sizeof(eh), 0) ||
	    memcmp(eh.e_ident, "\177ELF", 4) != 0 ||
	    eh.e_ident[4] != 2 /* ELFCLASS64 */ ||
	    eh.e_ident[5] != 1 /* ELFDATA2LSB */ ||
	    eh.e_machine != EM_RISCV) {
		fprintf(stderr, "rv: %s is not a riscv64 ELF file\n", path);
		goto out;
	}
	if ((eh.e_type != ET_EXEC && eh.e_type != ET_DYN) || (is_interp && eh.e_type != ET_DYN)) {
		fprintf(stderr, "rv: %s is not %s\n", path, is_interp ? "a shared object" : "an executable");
		goto out;
	}
	if (eh.e_phentsize != sizeof(struct elf64_phdr) || eh.e_phnum > MAX_PHDRS ||
	    read_at(fd, ph, (size_t)eh.e_phnum * sizeof(ph[0]), eh.e_phoff)) {
		fprintf(stderr, "rv: %s: cannot read the program headers\n", path);
		goto out;
	}

	for (unsigned i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP && !is_interp) {
			if (ph[i].p_filesz == 0 || ph[i].p_filesz > sizeof(img->interp) ||
			    read_at(fd, img->interp, ph[i].p_filesz, ph[i].p_offset) ||
			    img->interp[ph[i].p_filesz - 1] != '\0') {
				fprintf(stderr, "rv: %s: bad interpreter path\n", path);
				goto out;
			}
		}
		if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
			continue;
		if (ph[i].p_filesz > ph[i].p_memsz || ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
			fprintf(stderr, "rv: %s: bad segment sizes\n", path);
			goto out;
		}
		if (ph[i].p_vaddr < lo)
			lo = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > hi)
			hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (!hi) {
		fprintf(stderr, "rv: %s: no loadable segments\n", path);
		goto out;
	}

	if (eh.e_type == ET_DYN) {
		uint64_t first = lo & ~(RV_PAGE_SIZE - 1);

		if (is_interp) {
			uint64_t at = rv_mm_find_free(cpu, RV_PAGE_ALIGN(hi) - first);

			if (!at) {
				fprintf(stderr, "rv: %s does not fit the guest arena\n", path);
				goto out;
			}
			img->base = at - first;
		} else {
			img->base = RV_PIE_BASE - first;
		}
	}

	for (unsigned i = 0; i < eh.e_phnum; i++) {
		uint64_t vaddr = ph[i].p_vaddr + img->base;
		unsigned prot = 0;

		if (ph[i].p_type == PT_PHDR)
			img->phdr = vaddr;
		if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
			continue;
		if (ph[i].p_flags & PF_R)
			prot |= RV_PROT_READ;
		if (ph[i].p_flags & PF_W)
			prot |= RV_PROT_WRITE;
		if (ph[i].p_flags & PF_X)
			prot |= RV_PROT_EXEC;

		if (vaddr + ph[i].p_memsz > cpu->mmap_top ||
		    rv_mm_reserve(cpu, vaddr, ph[i].p_memsz, prot)) {
			fprintf(stderr, "rv: %s: segment at 0x%llx (+0x%llx) does not fit the guest's %llu MiB of addresses\n",
				path, (unsigned long long)vaddr, (unsigned long long)ph[i].p_memsz,
				(unsigned long long)(cpu->npages >> (20 - RV_PAGE_SHIFT)));
			goto out;
		}
		if (rv_mm_fill(cpu, vaddr, ph[i].p_filesz, NULL, fd, ph[i].p_offset)) {
			fprintf(stderr, "rv: %s: cannot load a segment\n", path);
			goto out;
		}
		rv_mm_name(cpu, vaddr, ph[i].p_filesz, fd, ph[i].p_offset);
		/* Without a PT_PHDR entry, find the headers inside the segment that holds them. */
		if (!img->phdr && eh.e_phoff >= ph[i].p_offset &&
		    eh.e_phoff < ph[i].p_offset + ph[i].p_filesz)
			img->phdr = vaddr + (eh.e_phoff - ph[i].p_offset);
	}

	img->entry = eh.e_entry + img->base;
	img->phnum = eh.e_phnum;
	img->end = hi + img->base;
	ret = 0;
out:
	close(fd);
	return ret;
}

/*
 * Where the program will end once loaded (load_image() above: a fixed
 * address executable where it was linked, a position-independent one at
 * RV_PIE_BASE), or 0 for anything that is not an executable (load_image()
 * says what is wrong with it).
 */
static uint64_t program_end(const char *path)
{
	static struct elf64_phdr ph[MAX_PHDRS];
	struct elf64_ehdr eh;
	uint64_t lo = UINT64_MAX, hi = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return 0;
	if (!read_at(fd, &eh, sizeof(eh), 0) && !memcmp(eh.e_ident, "\177ELF", 4) &&
	    (eh.e_type == ET_EXEC || eh.e_type == ET_DYN) &&
	    eh.e_phentsize == sizeof(struct elf64_phdr) && eh.e_phnum <= MAX_PHDRS &&
	    !read_at(fd, ph, (size_t)eh.e_phnum * sizeof(ph[0]), eh.e_phoff)) {
		for (unsigned i = 0; i < eh.e_phnum; i++) {
			if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
				continue;
			if (ph[i].p_vaddr < lo)
				lo = ph[i].p_vaddr;
			if (ph[i].p_vaddr + ph[i].p_memsz > hi)
				hi = ph[i].p_vaddr + ph[i].p_memsz;
		}
	}
	close(fd);
	if (hi && eh.e_type == ET_DYN)
		hi += RV_PIE_BASE - (lo & ~(RV_PAGE_SIZE - 1));
	return hi;
}

/*
 * Set up the guest's address space for running `path`: the stack area, the
 * program, and its interpreter if it names one. A dynamically linked program
 * starts in the interpreter, which finds the program through the auxiliary
 * vector and does the rest (loading libraries, relocating) as guest code.
 */
int rv_load_program(struct rv_cpu *cpu, const char *path, uint64_t stack_size)
{
	static char interp_path[RV_SYSROOT_MAX + 256];
	struct image prog, interp;
	uint64_t end;

	stack_size = RV_PAGE_ALIGN(stack_size);
	/* The stack goes at the top of the address space, which a large program may reach: more space first. */
	end = program_end(path);
	if (end && rv_mm_grow_to(cpu, RV_PAGE_ALIGN(end) + stack_size + 2 * RV_PAGE_SIZE)) {
		fprintf(stderr, "rv: %s does not fit the guest's address space\n", path);
		return -1;
	}
	if (stack_size + 16 * RV_PAGE_SIZE > cpu->npages << RV_PAGE_SHIFT) {
		fprintf(stderr, "rv: the guest's address space is too small for the stack\n");
		return -1;
	}
	cpu->stack_low = (cpu->npages << RV_PAGE_SHIFT) - stack_size;
	cpu->stack_size = stack_size;
	if (rv_mm_reserve(cpu, cpu->stack_low, stack_size, RV_PROT_READ | RV_PROT_WRITE))
		return -1;
	/* One unmapped page below the stack, so that running off it faults. */
	cpu->mmap_top = cpu->stack_low - RV_PAGE_SIZE;

	if (load_image(cpu, path, 0, &prog))
		return -1;
	cpu->pc = cpu->elf_entry = prog.entry;
	cpu->elf_phdr = prog.phdr;
	cpu->elf_phnum = prog.phnum;
	cpu->elf_base = 0;
	cpu->brk_start = cpu->proc->brk = RV_PAGE_ALIGN(prog.end);

	if (prog.interp[0]) {
		/* The interpreter is a guest file: look in the sysroot first. */
		snprintf(interp_path, sizeof(interp_path), "%s%s", cpu->sysroot, prog.interp);
		if (!cpu->sysroot[0] || access(interp_path, R_OK))
			snprintf(interp_path, sizeof(interp_path), "%s", prog.interp);
		if (load_image(cpu, interp_path, 1, &interp))
			return -1;
		cpu->pc = interp.entry;
		cpu->elf_base = interp.base;
	}
	return 0;
}

/*
 * Build the initial stack the way the Linux ELF loader does:
 *
 *   sp -> argc, argv[0..argc-1], NULL, envp..., NULL, auxv pairs, AT_NULL
 *
 * with the strings above it.
 */
int rv_setup_stack(struct rv_cpu *cpu, int argc, char **argv, char **envp)
{
	uint64_t top = (cpu->npages << RV_PAGE_SHIFT) & ~15ULL;
	uint64_t strp = top;
	uint64_t argv_guest[argc > 0 ? argc : 1];
	uint64_t random_guest, execfn_guest;
	uint64_t sp, *slot, *slots, env_bytes = 0, stack_size = (cpu->npages << RV_PAGE_SHIFT) - cpu->stack_low;
	uint8_t random[16];
	int ret;
	int nslots, envc = 0;

	/* Strings and pointers together may use a quarter of the stack. */
	while (envp && envp[envc]) {
		env_bytes += strlen(envp[envc]) + 1 + 8;
		envc++;
	}
	if (env_bytes > stack_size / 4) {
		fprintf(stderr, "rv: environment too large for the guest stack\n");
		return -1;
	}
	uint64_t envp_guest[envc > 0 ? envc : 1];


	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen(argv[i]) + 1;

		if (len > stack_size / 4)
			return -1;
		strp -= len;
		if (rv_mm_fill(cpu, strp, len, argv[i], -1, 0))
			return -1;
		argv_guest[i] = strp;
	}
	execfn_guest = argc > 0 ? argv_guest[0] : 0;
	for (int i = envc - 1; i >= 0; i--) {
		size_t len = strlen(envp[i]) + 1;

		strp -= len;
		if (rv_mm_fill(cpu, strp, len, envp[i], -1, 0))
			return -1;
		envp_guest[i] = strp;
	}

	/* AT_RANDOM: 16 bytes, fixed so that runs are reproducible. */
	strp -= 16;
	memset(random, 0x5a, sizeof(random));
	if (rv_mm_fill(cpu, strp, sizeof(random), random, -1, 0))
		return -1;
	random_guest = strp;

	nslots = 1 + argc + 1 + envc + 1 + 2 * 17;
	sp = (strp - (uint64_t)nslots * 8) & ~15ULL;
	if (sp < cpu->stack_low + RV_PAGE_SIZE)
		return -1;

	/* (Put together here first: the stack's pages need not lie in one piece.) */
	if (!(slots = slot = malloc((size_t)nslots * 8)))
		return -1;
	*slot++ = (uint64_t)argc;
	for (int i = 0; i < argc; i++)
		*slot++ = argv_guest[i];
	*slot++ = 0;			/* argv terminator */
	for (int i = 0; i < envc; i++)
		*slot++ = envp_guest[i];
	*slot++ = 0;			/* envp terminator */

#define AUX(type, value) do { *slot++ = (type); *slot++ = (value); } while (0)
	AUX(AT_PHDR, cpu->elf_phdr);
	AUX(AT_PHENT, sizeof(struct elf64_phdr));
	AUX(AT_PHNUM, cpu->elf_phnum);
	AUX(AT_PAGESZ, RV_PAGE_SIZE);
	AUX(AT_BASE, cpu->elf_base);
	AUX(AT_FLAGS, 0);
	AUX(AT_ENTRY, cpu->elf_entry);
	AUX(AT_UID, getuid());
	AUX(AT_EUID, geteuid());
	AUX(AT_GID, getgid());
	AUX(AT_EGID, getegid());
	AUX(AT_HWCAP, RV_HWCAP);
	AUX(AT_CLKTCK, 100);
	AUX(AT_SECURE, 0);
	AUX(AT_RANDOM, random_guest);
	AUX(AT_EXECFN, execfn_guest);
	AUX(AT_NULL, 0);
#undef AUX

	ret = rv_mm_fill(cpu, sp, (uint64_t)(slot - slots) * 8, slots, -1, 0);
	free(slots);
	cpu->x[2] = sp;
	return ret;
}
