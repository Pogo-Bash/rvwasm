/* Phase 1 target: static RV64 program, no libc, only write + exit. */
static long sys(long n, long a, long b, long c) {
    register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}
void _start(void) {
    static const char msg[] = "hello from riscv64\n";
    sys(64, 1, (long)msg, sizeof msg - 1);   /* write */
    sys(93, 0, 0, 0);                        /* exit  */
    for (;;) ;
}
