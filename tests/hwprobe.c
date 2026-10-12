/*
 * What riscv_hwprobe says: the base behaviour (key 3) and the extensions
 * (key 4: FD bit 0, C 1, V 2, Zba 3, Zbb 4, Zbs 5), as the kernel's
 * arch/riscv/include/uapi/asm/hwprobe.h numbers them.
 */
#include "guest.h"

#define SYS_riscv_hwprobe	258

void _start(void)
{
	struct { long key; u64 value; } pairs[2] = { { 3, 0 }, { 4, 0 } };

	put_hex("ret", (u64)sys(SYS_riscv_hwprobe, (long)pairs, 2, 0, 0, 0, 0));
	put_hex("base", pairs[0].value);
	put_hex("ima_ext_0", pairs[1].value);
	finish();
}
