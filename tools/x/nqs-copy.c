/* nqs-copy NQS BUILD FINAL: what nqs-run's per-boot loop does, in ONE process.
 *
 * nqs-run (on the nqs disk) copies the files of NQS/share/nqs/maps.list (lines "SIZE PATH", PATH relative to NQS/usr) into
 * /run/nqs the first time a native program starts: per file `rm`, `mkdir`, `dd` (seek: ftruncate gives the file its size, which
 * on this kernel's in-memory file system allocates one contiguous run, what Qt's MAP_SHARED needs) and `dd` (the content, notrunc).
 * That is 4 process creations per file, 598 in all, 46 s. This does the same with open/ftruncate/write in a single process.
 *
 * /init runs it in the background after the Debian mounts: BUILD (a directory in /debian/run, already holding the trees nqs-run
 * copies with cp -a) gets the files, then BUILD/ready, then BUILD is renamed to FINAL (rename(2) is atomic and fails if nqs-run
 * has made FINAL meanwhile, in which case nqs-run's own copy stands and this one is thrown away by the caller: a launch during
 * the copy never sees a half copy). Exit 0 only when FINAL is in place. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void mkdirs(char *p)
{
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0777);
			*s = '/';
		}
	}
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: nqs-copy NQS BUILD FINAL\n");
		return 2;
	}
	const char *nqs = argv[1], *build = argv[2], *final = argv[3];
	char path[4096], src[4096], line[4096];
	static char buf[65536];
	snprintf(path, sizeof path, "%s/share/nqs/maps.list", nqs);
	FILE *l = fopen(path, "r");
	if (!l) {
		perror(path);
		return 1;
	}
	long n = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (fgets(line, sizeof line, l)) {
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		*sp = 0;
		char *f = sp + 1;
		f[strcspn(f, "\r\n")] = 0;
		long long size = atoll(line);
		if (!*f) continue;
		snprintf(path, sizeof path, "%s/%s", build, f);
		snprintf(src, sizeof src, "%s/usr/%s", nqs, f);
		unlink(path);
		mkdirs(path);
		int out = open(path, O_WRONLY | O_CREAT, 0666);
		if (out < 0) {
			perror(path);
			return 1;
		}
		if (ftruncate(out, size) < 0) {
			perror("ftruncate");
			return 1;
		}
		int in = open(src, O_RDONLY);
		if (in < 0) {
			perror(src);
			return 1;
		}
		ssize_t r;
		while ((r = read(in, buf, sizeof buf)) > 0) {
			for (ssize_t o = 0; o < r;) {
				ssize_t w = write(out, buf + o, r - o);
				if (w < 0) {
					perror("write");
					return 1;
				}
				o += w;
			}
		}
		close(in);
		close(out);
		n++;
	}
	fclose(l);
	snprintf(path, sizeof path, "%s/ready", build);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0 || write(fd, "ready\n", 6) != 6) {
		perror(path);
		return 1;
	}
	close(fd);
	if (rename(build, final) < 0) {
		fprintf(stderr, "nqs-copy: %s -> %s: %s (%ld files copied)\n", build, final, strerror(errno), n);
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "nqs-copy: %ld files in %ld ms, %s ready\n", n, (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000), final);
	return 0;
}
