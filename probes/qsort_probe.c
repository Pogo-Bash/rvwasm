/* qsort() of a small array of 64-bit numbers on the stack, the way rv's hint_keep() did it, and what happened at the
 * lowest addresses of memory (the first 32 bytes, which on Linux/Wasm belong to nobody) meanwhile. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_pc(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static uint32_t low[8];
static void look(const char *what)
{
	uint32_t now[8];

	for (int i = 0; i < 8; i++)
		now[i] = *(volatile uint32_t *)(uintptr_t)(4 * i);
	if (memcmp(now, low, sizeof(now))) {
		printf("%s: the lowest words changed: ", what);
		for (int i = 0; i < 8; i++)
			printf("%x%s", (unsigned)now[i], i < 7 ? " " : "\n");
		memcpy(low, now, sizeof(low));
	} else {
		printf("%s: no change\n", what);
	}
}

static void sort_on_stack(int n)
{
	uint64_t pcs[128];
	static const uint64_t v[] = { 0xe7c61e, 0xe695f2, 0xe7ab6e, 0xe695f2, 0x100, 0x7, 0xabcdef, 0x42 };

	for (int k = 0; k < n; k++)
		pcs[k] = v[k % 8] + (uint64_t)(k / 8);
	qsort(pcs, (size_t)n, sizeof(pcs[0]), compare_pc);
	for (int k = 1; k < n; k++)
		if (pcs[k - 1] > pcs[k])
			printf("not sorted at %d\n", k);
	printf("sorted %d on the stack at %p\n", n, (void *)pcs);
}

int main(void)
{
	for (int i = 0; i < 8; i++)
		low[i] = *(volatile uint32_t *)(uintptr_t)(4 * i);
	printf("lowest words at the start:");
	for (int i = 0; i < 8; i++)
		printf(" %x", (unsigned)low[i]);
	printf("\n");
	sort_on_stack(3);
	look("qsort 3 on the stack");
	sort_on_stack(8);
	look("qsort 8 on the stack");
	sort_on_stack(100);
	look("qsort 100 on the stack");
	return 0;
}
