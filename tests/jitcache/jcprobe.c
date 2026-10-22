/* A native Linux/Wasm program that is not rv tries the store of translated code: it must be told there is none. */
#include <stdio.h>
#include <string.h>
long __rv_jit_cache(long op, const void *key, void *buf, unsigned long len);
int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	unsigned char key[16], rec[64], back[64];
	memset(key, 0x5a, sizeof(key));
	memset(rec, 0xa5, sizeof(rec));
	long put = __rv_jit_cache(1, key, rec, sizeof(rec));
	long get = __rv_jit_cache(0, key, back, sizeof(back));
	printf("jcprobe: put %ld, get %ld (-2: no store for this program)\n", put, get);
	return 0;
}
