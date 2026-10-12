/* Phase 0: trivial native wasm program for linux-wasm. */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	printf("hello from native wasm, pid %d, argc %d\n", (int)getpid(), argc);
	for (int i = 0; i < argc; i++)
		printf("  argv[%d] = %s\n", i, argv[i]);
	printf("  sizeof(void *) = %zu, sizeof(long) = %zu\n", sizeof(void *), sizeof(long));
	return 0;
}
