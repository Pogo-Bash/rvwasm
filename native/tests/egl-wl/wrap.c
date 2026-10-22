/*
 * VK-18: link with -Wl,--wrap=... (see build line in the card) to trace what Mesa asks of the kernel into a memory
 * ring (a hook on the host reads it live, because the console of a machine that dies loses its last lines).
 * The ring: vk18_trace[0..7] = bytes used, text from 16. Its address is printed once as "TRACEBUF 0x...".
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_munmap(void *, size_t);
int __real_ftruncate(int, off_t);
ssize_t __real_sendmsg(int, const struct msghdr *, int);
int __real_mkostemp(char *, int);
void *__real_memset(void *, int, size_t);
void *__real_memcpy(void *, const void *, size_t);
void *__real_memmove(void *, const void *, size_t);
void *__real___mmap(void *, size_t, int, int, int, off_t);

static char vk18_trace[1 << 17] __attribute__((aligned(16)));

void vk18_trace_announce(void)
{
	char b[64];
	int n = snprintf(b, sizeof b, "TRACEBUF 0x%lx\n", (unsigned long)vk18_trace);
	write(1, b, n);
}

void vk18_log(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof b - 1)
		n = sizeof b - 1;
	uint64_t *used = (uint64_t *)vk18_trace;
	uint64_t at = __atomic_fetch_add(used, (uint64_t)n, __ATOMIC_SEQ_CST);
	if (at + 16 + n > sizeof vk18_trace)
		return;
	__real_memcpy(vk18_trace + 16 + at, b, n);
}

void *__wrap_mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	void *r = __real_mmap(a, len, prot, flags, fd, off);
	vk18_log("mmap(len %zu prot %d flags 0x%x fd %d off %ld) = %p\n", len, prot, flags, fd, (long)off, r);
	return r;
}
void *__wrap___mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	void *r = __real___mmap(a, len, prot, flags, fd, off);
	if (len >= 65536 || fd >= 0)
		vk18_log("__mmap(len %zu prot %d flags 0x%x fd %d off %ld) = %p\n", len, prot, flags, fd, (long)off, r);
	return r;
}
int __wrap_munmap(void *a, size_t len)
{
	int r = __real_munmap(a, len);
	vk18_log("munmap(%p, %zu) = %d\n", a, len, r);
	return r;
}
int __wrap_ftruncate(int fd, off_t len)
{
	int r = __real_ftruncate(fd, len);
	vk18_log("ftruncate(%d, %ld) = %d\n", fd, (long)len, r);
	return r;
}
int __wrap_mkostemp(char *t, int f)
{
	int r = __real_mkostemp(t, f);
	vk18_log("mkostemp(%s) = %d\n", t, r);
	return r;
}
ssize_t __wrap_sendmsg(int fd, const struct msghdr *m, int flags)
{
	size_t n = 0;
	vk18_log("sendmsg(fd %d, control %zu, flags 0x%x):", fd, (size_t)m->msg_controllen, flags);
	for (size_t i = 0; i < m->msg_iovlen; i++) {
		const unsigned char *p = m->msg_iov[i].iov_base;
		size_t len = m->msg_iov[i].iov_len;
		/* wayland wire: id(4) opcode(2) size(2), body; the iov is a ring half, messages may straddle */
		for (size_t o = 0; o + 8 <= len;) {
			uint32_t id = *(const uint32_t *)(p + o);
			uint16_t op = *(const uint16_t *)(p + o + 4), sz = *(const uint16_t *)(p + o + 6);
			vk18_log(" [%u.%u/%u]", id, op, sz);
			if (sz < 8)
				break;
			o += sz;
		}
		n += len;
	}
	vk18_log(" %zu bytes\n", n);
	ssize_t r = __real_sendmsg(fd, m, flags);
	vk18_log("sendmsg = %zd\n", r);
	return r;
}

/* Writes through a NULL-ish pointer land in the kernel's image (no MMU): catch the library calls that do it. */
#define LOWMAX 0x400000UL
static void low_hit(const char *what, void *d, size_t n, const void *s)
{
	vk18_log("LOW %s(dst %p, n %zu, src %p): trapping\n", what, d, n, s);
	__builtin_trap();
}
void *__wrap_memset(void *d, int c, size_t n)
{
	if ((unsigned long)d < LOWMAX && n)
		low_hit("memset", d, n, (void *)(long)c);
	return __real_memset(d, c, n);
}
void *__wrap_memcpy(void *d, const void *s, size_t n)
{
	if ((unsigned long)d < LOWMAX && n)
		low_hit("memcpy", d, n, s);
	return __real_memcpy(d, s, n);
}
void *__wrap_memmove(void *d, const void *s, size_t n)
{
	if ((unsigned long)d < LOWMAX && n)
		low_hit("memmove", d, n, s);
	return __real_memmove(d, s, n);
}
