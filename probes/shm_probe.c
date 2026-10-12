/*
 * Does System V shared memory work on Linux/Wasm, and is a segment at the
 * same address in every process that attaches it (it should be, without an
 * MMU)? The first instance makes a segment, writes to it and starts a second
 * instance, which attaches it by its key and looks.
 */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define KEY	0x52560001
#define SIZE	(3 << 20)

int main(int argc, char **argv)
{
	struct shmid_ds ds;
	char *p;
	int id;

	if (argc > 1) {
		id = shmget(KEY, 0, 0);
		p = id < 0 ? (char *)-1 : shmat(id, NULL, SHM_RDONLY);
		printf("child: id %d, at %p (parent had %s), reads \"%s\"\n", id, (void *)p, argv[1],
		       p == (char *)-1 ? strerror(errno) : p + SIZE - 64);
		if (shmctl(id, IPC_STAT, &ds))
			printf("child: cannot ask about it: %s\n", strerror(errno));
		else
			printf("child: %lu attached\n", (unsigned long)ds.shm_nattch);
		return 0;
	}

	id = shmget(KEY, SIZE, IPC_CREAT | IPC_EXCL | 0600);
	if (id < 0) {
		printf("shmget: %s\n", strerror(errno));
		return 1;
	}
	p = shmat(id, NULL, 0);
	if (p == (char *)-1) {
		printf("shmat: %s\n", strerror(errno));
		shmctl(id, IPC_RMID, NULL);
		return 1;
	}
	strcpy(p + SIZE - 64, "written by the parent");
	if (shmctl(id, IPC_STAT, &ds))
		printf("parent: cannot ask about it: %s\n", strerror(errno));
	else
		printf("parent: %lu attached before the child\n", (unsigned long)ds.shm_nattch);
	{
		char addr[32];
		char *args[] = { argv[0], addr, NULL };
		pid_t pid;
		int status;

		snprintf(addr, sizeof(addr), "%p", (void *)p);
		if (posix_spawn(&pid, "/proc/self/exe", NULL, NULL, args, environ))
			printf("spawn failed\n");
		else
			waitpid(pid, &status, 0);
	}
	if (shmctl(id, IPC_STAT, &ds))
		printf("parent: cannot ask about it after the child is gone: %s\n", strerror(errno));
	else
		printf("parent: %lu attached after the child is gone\n", (unsigned long)ds.shm_nattch);
	printf("parent: still reads \"%s\"\n", p + SIZE - 64);
	shmdt(p);
	printf("removed: %d\n", shmctl(id, IPC_RMID, NULL));
	printf("gone: %s\n", shmget(KEY, 0, 0) < 0 ? "yes" : "no");
	return 0;
}
