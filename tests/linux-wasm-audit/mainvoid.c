/* A program whose main takes no arguments: valid C, cannot be started on linux-wasm (crt_arch.h). */
#include <stdio.h>
int main(void)
{
	puts("main(void) ran");
	return 0;
}
