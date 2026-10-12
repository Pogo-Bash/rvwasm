/*
 * Small pieces every native program gets (libnqcrt.a, linked by bin/nq-clang).
 * (The bridge for a main that takes nothing is crt/nqmain.c.)
 */

/*
 * dlsym: musl's wasm headers set _REDIR_TIME64, so <dlfcn.h> renames dlsym to
 * __dlsym_time64, which this musl does not define (time_t is 64-bit here
 * anyway). Static programs have no shared objects; dlsym answers as musl's
 * static stub does.
 */
void *dlsym(void *__restrict, const char *__restrict);

void *__dlsym_time64(void *__restrict handle, const char *__restrict name)
{
	return dlsym(handle, name);
}

/*
 * A C++ exception nothing catches leaves a Wasm program as a WebAssembly.Exception
 * the runtime reports without a word of what it was (with Wasm exception handling
 * there is no search phase that could see there is no handler and terminate).
 * Linking with -Wl,--wrap=__cxa_throw puts this in front of every throw: with
 * NQ_THROW_TRACE set in the environment, it says the thrown type's name first.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void __real___cxa_throw(void *object, void *type, void *(*destructor)(void *)) __attribute__((weak));

__attribute__((weak)) void __wrap___cxa_throw(void *object, void *type, void *(*destructor)(void *))
{
	static int trace = -1;
	if (trace < 0)
		trace = getenv("NQ_THROW_TRACE") != NULL;
	if (trace) {
		/* std::type_info: a vtable pointer, then the mangled name. */
		const char *name = *(const char **)((char *)type + sizeof(void *));
		write(2, "nq: throw ", 10);
		write(2, name, strlen(name));
		write(2, "\n", 1);
	}
	__real___cxa_throw(object, type, destructor);
}

/*
 * The same for every raise, a rethrow included (link with
 * -Wl,--wrap=_Unwind_RaiseException): the exception's class, and for a C++
 * one the type, read from libc++abi's __cxa_exception in front of the unwind
 * header (wasm64: exceptionType is 80 bytes before it).
 */
int __real__Unwind_RaiseException(void *header) __attribute__((weak));

__attribute__((weak)) int __wrap__Unwind_RaiseException(void *header)
{
	if (getenv("NQ_THROW_TRACE")) {
		char class[9] = { 0 };
		memcpy(class, header, 8);
		write(2, "nq: raise ", 10);
		write(2, class, strlen(class));
		if (memcmp(class, "CLNGC++", 7) == 0) {
			void *type = *(void **)((char *)header - 80);
			const char *name = *(const char **)((char *)type + sizeof(void *));
			write(2, " ", 1);
			write(2, name, strlen(name));
		}
		write(2, "\n", 1);
	}
	return __real__Unwind_RaiseException(header);
}

/*
 * Link with -Wl,--wrap=malloc to hear of every malloc that fails (std::bad_alloc
 * otherwise says nothing of the size). On this kernel (no MMU) a large
 * allocation needs that many contiguous pages, rounded up to a power of two.
 */
#include <errno.h>

void *__real_malloc(size_t size) __attribute__((weak));

static void nq_say_number(unsigned long long n)
{
	char text[24];
	int i = sizeof text;
	do
		text[--i] = '0' + n % 10;
	while ((n /= 10) && i > 0);
	write(2, text + i, sizeof text - i);
}

__attribute__((weak)) void *__wrap_malloc(size_t size)
{
	void *p = __real_malloc(size);
	if (!p && size) {
		int saved = errno;
		write(2, "nq: malloc(", 11);
		nq_say_number(size);
		write(2, ") failed, errno ", 16);
		nq_say_number(saved);
		write(2, "\n", 1);
		errno = saved;
	}
	return p;
}

/*
 * musl's __map_file maps a file MAP_SHARED, which fails for a file on a disk here (no MMU: only ramfs/shmem
 * files can be shared, wlserver/README.md). Its callers are the time zone files (TZ=America/Chicago, or
 * /etc/localtime), which a failed map leaves as UTC: every program of the system showed UTC through localtime()
 * (QDateTime::currentDateTime(), QML's Date.getHours(): Fuzzy Clock) while Qt's own zone database (QTimeZone,
 * the time data engine, the panel's clock) read the files and was right. This one reads the file into memory
 * instead (the same contract: the bytes, their size, 0 for failure; callers munmap them). libnqcrt.a comes
 * before libc.a on the link line, so this one is the one that is linked.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

const unsigned char *__map_file(const char *pathname, size_t *size)
{
	int fd = open(pathname, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	struct stat st;
	unsigned char *map = 0;
	if (!fstat(fd, &st) && st.st_size > 0) {
		void *m = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m != MAP_FAILED) {
			size_t got = 0;
			ssize_t n;
			while (got < (size_t)st.st_size && (n = read(fd, (char *)m + got, st.st_size - got)) > 0)
				got += n;
			if (got == (size_t)st.st_size) {
				map = m;
				*size = st.st_size;
			} else {
				munmap(m, st.st_size);
			}
		}
	}
	close(fd);
	return map;
}
