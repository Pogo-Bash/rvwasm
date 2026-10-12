/* struct stat layout: musl's wasm kstat against the kernel's asm-generic struct stat. */
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct stat st;
	int fd = open("/tmp/statprobe", O_CREAT | O_WRONLY | O_TRUNC, 0640);
	write(fd, "hello world\n", 12);
	close(fd);
	link("/tmp/statprobe", "/tmp/statprobe2");
	memset(&st, 0xa5, sizeof st);
	if (stat("/tmp/statprobe", &st)) { perror("stat"); return 1; }
	printf("sizeof(nlink_t)=%zu sizeof(blksize_t)=%zu off(st_nlink)=%zu off(st_uid)=%zu\n",
	       sizeof(nlink_t), sizeof(blksize_t), offsetof(struct stat, st_nlink), offsetof(struct stat, st_uid));
	printf("mode=%o (want 100640) nlink=%lu (want 2) uid=%u gid=%u size=%lld (want 12) blksize=%ld blocks=%lld mtime=%lld\n",
	       (unsigned)st.st_mode, (unsigned long)st.st_nlink, st.st_uid, st.st_gid,
	       (long long)st.st_size, (long)st.st_blksize, (long long)st.st_blocks, (long long)st.st_mtime);
	unlink("/tmp/statprobe2");
	unlink("/tmp/statprobe");
	return 0;
}
