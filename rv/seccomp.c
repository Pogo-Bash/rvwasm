/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * System call filters (seccomp).
 *
 * A program hands the kernel a small program of its own, in the instruction
 * set of the classic packet filter, which is from then on asked about every
 * system call the program makes: let it through, fail it with an error, send
 * the caller a signal instead (SIGSYS, whose handler may do the call's work
 * some other way: how a browser's sandboxed processes open files, through a
 * broker), or kill. Filters only ever add up, are inherited by threads and
 * by children, and stay across exec.
 *
 * The host's kernel cannot do this for the guest. Its filters see the system
 * calls of the process, and those are the emulator's: other calls than the
 * guest's, more of them, under another architecture's name. So the filters
 * are kept and run here, on the guest's calls as the guest makes them, with
 * the architecture the guest expects to see (riscv64). What the guest
 * cannot do is make a call that does not come through here.
 *
 * Every thread has the chain of filters that apply to it (the newest first,
 * each pointing at the one before); chains share their tails, and a filter
 * once made is never changed or freed. A child made by starting a second
 * emulator copies its parent's chain (rv_seccomp_fork_child()); across exec
 * the chain travels through a descriptor that only the next emulator reads
 * (rv_seccomp_save(), rv_seccomp_load()).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "rv.h"

/* One instruction (struct sock_filter). */
struct insn {
	uint16_t code;
	uint8_t jt, jf;
	uint32_t k;
};

struct rv_filter {
	struct rv_filter *prev;
	uint32_t len;
	struct insn insns[];
};

#define MAX_INSNS	4096	/* BPF_MAXINSNS */
#define MAX_CHAIN	32768	/* MAX_INSNS_PER_PATH: all of a thread's filters together, each one counting 4 more */
#define DATA_SIZE	64	/* struct seccomp_data: nr, arch, instruction_pointer, args[6] */
#define ARCH_RISCV64	0xc00000f3U	/* AUDIT_ARCH_RISCV64 */

#define CLASS(code)	((code) & 0x07)
#define C_LD	0x00
#define C_LDX	0x01
#define C_ST	0x02
#define C_STX	0x03
#define C_ALU	0x04
#define C_JMP	0x05
#define C_RET	0x06
#define C_MISC	0x07

/* The kernel's rules for a filter (seccomp_check_filter()): only loads of whole words of the call's data, jumps that stay inside, a return at the end. */
static int valid(const struct insn *f, uint32_t len)
{
	if (!len || len > MAX_INSNS || CLASS(f[len - 1].code) != C_RET)
		return 0;
	for (uint32_t i = 0; i < len; i++) {
		uint16_t code = f[i].code;
		uint32_t k = f[i].k;

		switch (code) {
		case 0x20:	/* ld [k] */
			if (k >= DATA_SIZE || (k & 3))
				return 0;
			break;
		case 0x60: case 0x61: case 0x02: case 0x03:	/* ld M[k], ldx M[k], st, stx */
			if (k >= 16)
				return 0;
			break;
		case 0x00: case 0x01: case 0x80: case 0x81:	/* ld #k, ldx #k, ld len, ldx len */
		case 0x06: case 0x16:	/* ret k, ret a */
		case 0x07: case 0x87:	/* tax, txa */
		case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x44: case 0x4c:
		case 0x54: case 0x5c: case 0x64: case 0x6c: case 0x74: case 0x7c: case 0x84:
		case 0xa4: case 0xac: case 0x3c: case 0x9c:	/* add sub mul or and lsh rsh neg xor, div and mod by x */
			break;
		case 0x34: case 0x94:	/* div and mod by a constant */
			if (!k)
				return 0;
			break;
		case 0x05:	/* ja */
			if (k >= len - i - 1)
				return 0;
			break;
		case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d: case 0x45: case 0x4d:	/* jeq jgt jge jset */
			if (f[i].jt >= len - i - 1 || f[i].jf >= len - i - 1)
				return 0;
			break;
		default:
			return 0;
		}
	}
	return 1;
}

static uint32_t run(const struct rv_filter *filter, const uint32_t *data)
{
	uint32_t a = 0, x = 0, mem[16] = { 0 };
	const struct insn *f = filter->insns;

	for (uint32_t pc = 0; pc < filter->len; pc++) {
		uint32_t k = f[pc].k, src = (f[pc].code & 0x08) ? x : k;

		switch (f[pc].code) {
		case 0x20: a = data[k / 4]; break;
		case 0x60: a = mem[k]; break;
		case 0x61: x = mem[k]; break;
		case 0x02: mem[k] = a; break;
		case 0x03: mem[k] = x; break;
		case 0x00: a = k; break;
		case 0x01: x = k; break;
		case 0x80: a = DATA_SIZE; break;
		case 0x81: x = DATA_SIZE; break;
		case 0x06: return k;
		case 0x16: return a;
		case 0x07: x = a; break;
		case 0x87: a = x; break;
		case 0x04: case 0x0c: a += src; break;
		case 0x14: case 0x1c: a -= src; break;
		case 0x24: case 0x2c: a *= src; break;
		case 0x34: case 0x3c:
			if (!src)
				return 0;	/* (as the kernel: the filter fails, and that kills) */
			a /= src;
			break;
		case 0x94: case 0x9c:
			if (!src)
				return 0;
			a %= src;
			break;
		case 0x44: case 0x4c: a |= src; break;
		case 0x54: case 0x5c: a &= src; break;
		case 0x64: case 0x6c: a = src < 32 ? a << src : 0; break;
		case 0x74: case 0x7c: a = src < 32 ? a >> src : 0; break;
		case 0x84: a = -a; break;
		case 0xa4: case 0xac: a ^= src; break;
		case 0x05: pc += k; break;
		case 0x15: case 0x1d: pc += a == src ? f[pc].jt : f[pc].jf; break;
		case 0x25: case 0x2d: pc += a > src ? f[pc].jt : f[pc].jf; break;
		case 0x35: case 0x3d: pc += a >= src ? f[pc].jt : f[pc].jf; break;
		case 0x45: case 0x4d: pc += (a & src) ? f[pc].jt : f[pc].jf; break;
		}
	}
	return 0;
}

/*
 * What the thread's filters say about the system call `nr` with the
 * arguments args[0..5], made by the ecall at cpu->pc: the answer that
 * weighs most, as one of the kernel's SECCOMP_RET_ values with its data.
 */
uint32_t rv_seccomp_check(struct rv_cpu *cpu, uint64_t nr, const uint64_t *args)
{
	uint32_t data[DATA_SIZE / 4], verdict = RV_SECCOMP_ALLOW;
	uint64_t after = cpu->pc + 4;

	if (cpu->seccomp_strict) {
		/* The old mode: read, write, exit and the return from a signal handler, or death. */
		if (nr != 63 && nr != 64 && nr != 93 && nr != 139)
			return RV_SECCOMP_KILL_STRICT;
	}
	data[0] = (uint32_t)nr;
	data[1] = ARCH_RISCV64;
	data[2] = (uint32_t)after;
	data[3] = (uint32_t)(after >> 32);
	for (int i = 0; i < 6; i++) {
		data[4 + 2 * i] = (uint32_t)args[i];
		data[5 + 2 * i] = (uint32_t)(args[i] >> 32);
	}
	/* All of them are asked, and the least permissive answer counts (their order as signed numbers). */
	for (const struct rv_filter *f = cpu->seccomp; f; f = f->prev) {
		uint32_t ret = run(f, data);

		if ((int32_t)(ret & 0xffff0000U) < (int32_t)(verdict & 0xffff0000U))
			verdict = ret;
	}
	return verdict;
}

/* Whether the kernel would let this task install a filter: it has said that it will gain no privileges, or it is root. */
static int may_filter(void)
{
	return prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) == 1 || geteuid() == 0;
}

static struct rv_filter *make(const struct insn *insns, uint32_t len, struct rv_filter *prev)
{
	struct rv_filter *f = malloc(sizeof(*f) + len * sizeof(*insns));

	if (!f)
		return NULL;
	f->prev = prev;
	f->len = len;
	memcpy(f->insns, insns, len * sizeof(*insns));
	return f;
}

/* seccomp(SECCOMP_SET_MODE_FILTER, flags, prog) and prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, prog). */
static uint64_t set_filter(struct rv_cpu *cpu, uint64_t flags, uint64_t prog_addr)
{
	const uint8_t *prog = rv_user_ptr(cpu, prog_addr, 16, RV_PROT_READ);
	struct rv_filter *f;
	const struct insn *insns;
	uint64_t insns_addr;
	uint32_t total;
	uint16_t len;

	/* TSYNC, LOG, SPEC_ALLOW and TSYNC_ESRCH; not a listener for another process to answer by. */
	if (flags & ~(uint64_t)(1 | 2 | 4 | 16))
		return (uint64_t)-(int64_t)RV_EINVAL;
	if (!prog)
		return (uint64_t)-(int64_t)RV_EFAULT;
	memcpy(&len, prog, 2);
	memcpy(&insns_addr, prog + 8, 8);
	if (!len || len > MAX_INSNS)
		return (uint64_t)-(int64_t)RV_EINVAL;
	insns = rv_user_ptr(cpu, insns_addr, (uint64_t)len * sizeof(*insns), RV_PROT_READ);
	if (!insns)
		return (uint64_t)-(int64_t)RV_EFAULT;
	if (!valid(insns, len))
		return (uint64_t)-(int64_t)RV_EINVAL;
	if (!may_filter())
		return (uint64_t)-(int64_t)RV_EACCES;

	rv_lock(cpu);
	/* The kernel's bound on what one call can be made to run through (and on what travels across an exec). */
	total = len;
	for (const struct rv_filter *p = cpu->seccomp; p; p = p->prev)
		total += p->len + 4;
	if (total > MAX_CHAIN) {
		rv_unlock(cpu);
		return (uint64_t)-(int64_t)RV_ENOMEM;
	}
	f = make(insns, len, cpu->seccomp);
	if (!f) {
		rv_unlock(cpu);
		return (uint64_t)-(int64_t)RV_ENOMEM;
	}
	cpu->seccomp = f;
	if (flags & 1) {
		/* TSYNC: every thread of the process, now. (The kernel wants their chains to be beginnings of ours; here they simply become ours.) */
		for (struct rv_cpu *t = cpu->proc->threads; t; t = t->next_thread) {
			t->seccomp = f;
			t->seccomp_strict = cpu->seccomp_strict;
		}
	}
	rv_unlock(cpu);
	return 0;
}

/* The seccomp() system call. */
uint64_t rv_seccomp(struct rv_cpu *cpu, uint64_t op, uint64_t flags, uint64_t arg)
{
	switch (op) {
	case 0:	/* SECCOMP_SET_MODE_STRICT */
		if (flags || arg)
			return (uint64_t)-(int64_t)RV_EINVAL;
		if (cpu->seccomp)
			return (uint64_t)-(int64_t)RV_EINVAL;
		cpu->seccomp_strict = 1;
		return 0;
	case 1:	/* SECCOMP_SET_MODE_FILTER */
		if (cpu->seccomp_strict)
			return (uint64_t)-(int64_t)RV_EINVAL;
		return set_filter(cpu, flags, arg);
	case 2: {	/* SECCOMP_GET_ACTION_AVAIL */
		const uint8_t *p = rv_user_ptr(cpu, arg, 4, RV_PROT_READ);
		uint32_t action;

		if (flags)
			return (uint64_t)-(int64_t)RV_EINVAL;
		if (!p)
			return (uint64_t)-(int64_t)RV_EFAULT;
		memcpy(&action, p, 4);
		switch (action) {
		case RV_SECCOMP_KILL_PROCESS: case RV_SECCOMP_KILL_THREAD: case RV_SECCOMP_TRAP:
		case RV_SECCOMP_ERRNO: case RV_SECCOMP_TRACE: case RV_SECCOMP_LOG: case RV_SECCOMP_ALLOW:
			return 0;
		}
		return (uint64_t)-(int64_t)95;	/* EOPNOTSUPP */
	}
	default:
		return (uint64_t)-(int64_t)RV_EINVAL;
	}
}

/* prctl(PR_SET_SECCOMP, mode, prog) and prctl(PR_GET_SECCOMP). */
uint64_t rv_seccomp_prctl(struct rv_cpu *cpu, int set, uint64_t mode, uint64_t arg)
{
	if (!set)
		return cpu->seccomp ? 2 : cpu->seccomp_strict ? 1 : 0;
	if (mode == 1)
		return rv_seccomp(cpu, 0, 0, 0);
	if (mode == 2)
		return rv_seccomp(cpu, 1, 0, arg);
	return (uint64_t)-(int64_t)RV_EINVAL;
}

/*
 * In the child of a fork that started a second emulator: cpu->seccomp is the
 * parent's chain, in the parent's memory. A copy of it, of our own. 0, or -1.
 */
int rv_seccomp_fork_child(struct rv_cpu *cpu, int (*copy)(void *dst, const void *src, size_t len))
{
	struct rv_filter *theirs = cpu->seccomp, **link = &cpu->seccomp;

	*link = NULL;
	while (theirs) {
		struct rv_filter head, *mine;

		if (copy(&head, theirs, sizeof(head)) || head.len > MAX_INSNS ||
		    !(mine = malloc(sizeof(*mine) + head.len * sizeof(struct insn))))
			return -1;
		if (copy(mine, theirs, sizeof(*mine) + head.len * sizeof(struct insn))) {
			free(mine);
			return -1;
		}
		mine->prev = NULL;
		*link = mine;
		link = &mine->prev;
		theirs = head.prev;
	}
	return 0;
}

/*
 * Across exec: the next program runs in a new emulator, which must begin
 * with this thread's filters. They go as one image: the strict flag, the
 * number of filters, and then each filter's length and instructions, all
 * 32-bit words, newest filter first. The exec writes the image to a
 * descriptor the new emulator inherits (sys_execve() finds one: a file in
 * memory, a pipe, a file without a name) and names the descriptor to it as
 * RV_SECCOMP in its environment, which is ours to set: the guest's own
 * environment for the new program never has that name in it. The image
 * says its own size, so the reader never has to wait for an end of file.
 * Returns the image, malloc()ed, with its size; NULL and size 0 when there
 * is nothing to pass on; NULL and size (size_t)-1 when it cannot be made
 * (and then the exec must not happen).
 */
void *rv_seccomp_save(struct rv_cpu *cpu, size_t *size)
{
	uint32_t *image, *p, count = 0;
	size_t words = 2;

	*size = 0;
	if (!cpu->seccomp && !cpu->seccomp_strict)
		return NULL;
	for (const struct rv_filter *f = cpu->seccomp; f; f = f->prev) {
		words += 1 + (size_t)f->len * (sizeof(struct insn) / 4);
		count++;
	}
	image = malloc(words * 4);
	if (!image) {
		*size = (size_t)-1;
		return NULL;
	}
	p = image;
	*p++ = (uint32_t)cpu->seccomp_strict;
	*p++ = count;
	for (const struct rv_filter *f = cpu->seccomp; f; f = f->prev) {
		*p++ = f->len;
		memcpy(p, f->insns, (size_t)f->len * sizeof(struct insn));
		p += (size_t)f->len * (sizeof(struct insn) / 4);
	}
	*size = words * 4;
	return image;
}

/* As many bytes as asked for, from a descriptor that may give them in pieces (a pipe). 0, or -1. */
static int read_whole(int fd, void *buf, size_t len)
{
	while (len) {
		ssize_t n = read(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		buf = (char *)buf + n;
		len -= (size_t)n;
	}
	return 0;
}

/* At the start of an emulator: the filters its predecessor left for it, if any. 0, or -1 if they are there and cannot be had. */
int rv_seccomp_load(struct rv_cpu *cpu)
{
	const char *env = getenv("RV_SECCOMP");
	struct rv_filter **link = &cpu->seccomp;
	uint32_t head[2], word;
	int fd, ret = -1;

	if (!env)
		return 0;
	fd = atoi(env);
	unsetenv("RV_SECCOMP");
	if (fd < 3)
		goto out;
	/* A file is read from its beginning; a pipe has none, and is as it is. */
	lseek(fd, 0, SEEK_SET);
	if (read_whole(fd, head, 8) || head[0] > 1)
		goto out;
	cpu->seccomp_strict = (int)head[0];
	for (uint32_t i = 0; i < head[1]; i++) {
		struct rv_filter *f;

		if (read_whole(fd, &word, 4) || !word || word > MAX_INSNS ||
		    !(f = malloc(sizeof(*f) + word * sizeof(struct insn))))
			goto out;
		f->prev = NULL;
		f->len = word;
		if (read_whole(fd, f->insns, word * sizeof(struct insn)) || !valid(f->insns, word)) {
			free(f);
			goto out;
		}
		*link = f;
		link = &f->prev;
	}
	ret = 0;
out:
	if (fd >= 3)
		close(fd);
	return ret;
}
