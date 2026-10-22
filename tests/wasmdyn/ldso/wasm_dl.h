/*
 * The interface between the dynamic linker (ldso.c, in the program's own
 * process) and the host (the runtime's __wasm_dl import, patch-runtime.py item 26).
 *
 * The host does only what only the host can do: compile a module, make the
 * objects it imports (globals with given values, functions taken from the
 * table or from the exports of a module instantiated before), instantiate it,
 * and put its function exports into the function table. Which file, where its
 * memory and table slots go, and what every import is bound to are decided
 * here, in C, by the dynamic linker.
 *
 * Every Worker of a process (every thread) has its own instances and its own
 * table, so each thread must load every module itself, in the same order,
 * with the same descriptor: the descriptors are kept in a log in memory, and
 * a thread replays the part of it it has not seen (__dl_sync()).
 *
 * All fields are 64-bit little-endian words (the host reads them with a
 * DataView), whatever the field's natural size.
 */
#ifndef WASM_DL_H
#define WASM_DL_H

#include <stdint.h>

/* __wasm_dl(op, arg) */
#define WASM_DL_LOAD	0	/* arg: struct wasm_dl_desc *; returns the new instance's id (1, 2, ...) or -1 */
#define WASM_DL_COUNT	1	/* returns how many modules this Worker has loaded (ids 1..count) */
#define WASM_DL_TABLE	2	/* returns the length of this Worker's function table */

/* What an import is bound to: one entry per import, in the module's import order. */
#define WASM_DL_HOST	1	/* the object the runtime gives the program under that name (a = name) */
#define WASM_DL_GLOBAL	2	/* a new global: type = valtype (0x7f i32, 0x7e i64) | 0x100 if mutable; a = value */
#define WASM_DL_SLOT	3	/* the function in table slot a */
#define WASM_DL_EXPORT	4	/* export b (name) of instance a (0: the program the kernel started) */
#define WASM_DL_MISSING	5	/* a function that traps with "undefined symbol: a" when called */

struct wasm_dl_import {
	uint64_t kind;
	uint64_t type;
	uint64_t a;
	uint64_t b;
};

struct wasm_dl_desc {
	uint64_t bytes;		/* the module (in memory for the process's lifetime: threads started later load it too) */
	uint64_t len;		/* 0: no module, only `place` for instance `module` */
	uint64_t nimports;
	uint64_t imports;	/* struct wasm_dl_import[nimports] */
	uint64_t table_min;	/* grow the table to at least this many slots first */
	uint64_t place;		/* the function exports, in export order, go into slots place, place + 1, ... (0: none) */
	uint64_t module;	/* with len 0: the instance whose exports are placed */
	uint64_t us_compile;	/* written by the host: microseconds it took to compile (or find) the module, */
	uint64_t us_instantiate;	/* to instantiate it, */
	uint64_t us_place;	/* and to place exports */
	char key[32];		/* the module's content key (a string), written by the host on the first load; later loads of the
				 * same entry (a thread replaying the log) use it and do not hash the bytes again */
};

#endif
