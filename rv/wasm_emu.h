/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The native build's stand-in for the browser's WebAssembly engine (wasm_emu.c):
 * runs the JIT's modules by interpretation, so that translated code can be
 * tested where there is no engine. RV_JIT_EMU=1; see jit.c.
 */
#ifndef WASM_EMU_H
#define WASM_EMU_H

#include <stdint.h>

struct rv_cpu;

long rv_emu_install(const void *code, unsigned long len, long slot);
int rv_emu_run(long slot, struct rv_cpu *cpu, uint32_t entry, uint32_t budget);
void rv_emu_report(void);
void rv_emu_thread_exit(void);

#endif /* WASM_EMU_H */
