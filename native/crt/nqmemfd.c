/*
 * memfd_create() for a kernel without it. The machine's kernel is a no-MMU one without shmem
 * (CONFIG_MEMFD_CREATE unset), so the system call fails with ENOSYS. Mesa's wl_shm buffers
 * (dri2_wl_swrast_allocate_buffer) and Wayland clients in general ask for a memfd; when it fails Mesa 25.0's
 * EGL swap path goes on with a NULL buffer and memset()s it (dri2_wl_swrast_get_image), and with no MMU a write
 * to address 0 is a write to the kernel's own image: sys_call_table, the console's operations and the network
 * driver's all became zeroes (docs/native-wasm.md, VK-18). This stands in for it: a file in $XDG_RUNTIME_DIR
 * (or /tmp) that is unlinked at once, which is what Mesa's and Wayland's own fallbacks do and what a memory
 * file system (ramfs, what tmpfs is here) hands out as a mappable file. Seals are not available on it.
 * Its own archive member of libnqcrt.a: libc's memfd_create is then not pulled in.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int memfd_create(const char *name, unsigned flags)
{
	long r = syscall(SYS_memfd_create, name, (long)flags);
	if (r >= 0 || errno != ENOSYS)
		return r < 0 ? -1 : (int)r;
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[256];
	if (!dir || !*dir)
		dir = "/tmp";
	int n = snprintf(path, sizeof path, "%s/memfd-%s-XXXXXX", dir, name ? name : "anon");
	if (n <= 0 || n >= (int)sizeof path)
		return (errno = ENAMETOOLONG), -1;
	int fd = mkostemp(path, (flags & MFD_CLOEXEC) ? O_CLOEXEC : 0);
	if (fd < 0)
		return -1;
	unlink(path);
	return fd;
}
