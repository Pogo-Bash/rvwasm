# Dynamic libraries for native wasm programs on linux-wasm

Status 2026-10-12: research, design and a working prototype (`tests/wasmdyn/`). The host's part is in
the runtime as item 26 of `tools/patch-runtime.py` (off unless the page has `?dl=1`), with a per-page
module cache ([section 10](#10-the-host-in-the-runtime-and-the-per-page-module-cache)); the rest is not
in the machine yet.

## Summary

**Feasible.** Shared libraries and `dlopen` work on linux-wasm with the WebAssembly tool conventions'
format (`dylink.0`, `wasm-ld -shared` / `-pie`), a dynamic linker that lives in the process like
Linux's `ld.so`, and one small, generic host primitive in the runtime. The prototype runs, under
node-machine on the real kernel and runtime:

- a C++ program linked against a shared library (calls and data both ways, function pointers both
  ways, virtual calls across the boundary, a subclass of the library's class overriding its virtual
  function, `dynamic_cast` in either module on either module's objects, exceptions thrown in one
  module and caught by base type in the other, in both directions, the library's constructors and
  thread-local variables);
- `dlopen`/`dlsym`/`dlclose`/`dladdr`/`dlerror` with RTLD_LOCAL and RTLD_GLOBAL;
- a thread started *before* `dlopen` calling into the library loaded after it (each thread with its
  own copy of the library's thread-local variable), and a thread started after;
- a vfork-style child (`clone(CLONE_VM | CLONE_VFORK)`, as posix_spawn and Qt's forkfd make them)
  calling into a `dlopen`ed library on its parent's TLS, and `posix_spawn`.

All 50 checks pass (`tests/wasmdyn/README.md` says how to run them).

**Recommended design, in brief.**

1. *Format:* the tool conventions' dynamic linking ABI as LLVM implements it. Libraries are
   `wasm-ld -shared`, programs `wasm-ld -pie` (as now). What LLVM 18 lacks is listed under
   [Format](#1-format-what-llvm-gives-and-what-is-missing); none of it blocks.
2. *The dynamic linker is musl's, in the process* (`ld-musl-wasm64.so.1` = `libc.so`, as on every
   musl system), not the JavaScript runtime and not the kernel. It decides everything: which file,
   where its data goes (`mmap`), which table slots it gets, what every import is bound to (symbol
   lookup in ELF order), TLS, constructors, `dl*` semantics. The host gets one import,
   `__wasm_dl(op, arg)`: "compile these bytes, instantiate them with these bindings, remember the
   instance". That is the only thing a process cannot do itself.
3. *Threads:* every Worker has its own instances and its own table, so every load goes into a log in
   shared memory that each thread replays (same descriptors, same order, so the same table slots
   everywhere). A new task replays before its first instruction (the runtime calls a hook, later
   musl's `__libc_clone_callback` itself); existing threads are made to replay by `dlopen` through
   musl's own `__synccall` (a signal to every thread whose handler replays). No kernel change.
4. *Exceptions, longjmp, RTTI* work across modules with one tag object per process per Worker (the C
   library's, imported by everyone) and ELF-style first-definition-wins binding of `GOT.mem` (which
   makes typeinfo unique).
5. *Each module's exported functions are in the table by its own element segment and its symbols
   are in a custom section instead of wasm exports* (`tablify.py --dynsym`, a post-link step that
   should become a wasm-ld option). V8 makes a JavaScript function object per export per instance:
   20,000 exports cost 9 ms per thread start, the same library without them 0.9 to 2.5 ms.

**Effort to Qt/KDE with plugins loaded at run time:** about 4 to 6 weeks of agent time; the first Qt
program that `dlopen`s its platform plugin in about 2 weeks. Per piece in [the plan](#7-plan-pieces-effort-upstreaming).
The biggest risk is not correctness but the cost of a new thread in a process with many large
libraries (every Worker decodes and instantiates every library); measured and discussed under
[Performance](#6-performance).

## 0. Where we start

How a native program runs today (read in the runtime, `linux-wasm/runtime/linux-worker.js`, with
`tools/patch-runtime.py` on top, and the kernel's `fs/binfmt_wasm.c` and `arch/wasm/kernel/process.c`):

- `binfmt_wasm` accepts a module whose first section is `dylink.0`, reads its `MEM_INFO`
  (memory size and alignment, table size), maps the whole file (`start_code..end_code`), allocates
  the data area (`start_data`) and a stack. `start_thread()` calls the host's
  `wasm_load_executable(bin_start, bin_end, data_start, table_start=0)`.
- The runtime copies the bytes out of shared memory (`slice`: a module cannot be compiled from a
  SharedArrayBuffer), compiles them, and instantiates with `env.memory` (the one shared memory64),
  `__memory_base` (= data_start), `__table_base` 0, `__stack_pointer`, a *new*
  `__indirect_function_table` of its own, and `__wasm_syscall_0..6`.
- A thread (`clone`) is a new Worker: `wasm_create_and_run_task` hands it `bin_start..bin_end`
  again, and it compiles and instantiates the same module with the same `__memory_base` and its
  own table, filled the same way by the element segments, so function pointers mean the same in
  every thread. Wasm has no shared tables (that is the shared-everything-threads proposal, phase 1
  in the WebAssembly proposals list), so per-Worker tables are not a choice we have.
- Programs are static PIEs (`nativeqt/bin/nq-clang`: `wasm-ld --experimental-pic -pie`); the
  decision and its costs are in `docs/native-wasm.md` ("Linking: one static module per program"),
  which also names what dynamic linking would need. This document is that.

## 1. Format: what LLVM gives and what is missing

The format is the tool conventions' (WebAssembly/tool-conventions `DynamicLinking.md`):

- `dylink.0` first: `MEM_INFO` (memory and table size and alignment), `NEEDED`, `EXPORT_INFO`
  (TLS exports), `IMPORT_INFO` (weak imports), `RUNTIME_PATH`.
- Imports: `env.memory`, `env.__indirect_function_table`, `env.__stack_pointer`,
  `env.__memory_base`, `env.__table_base`; functions called directly are function imports from
  `env`; addresses of other modules' data and functions are mutable globals imported from
  `GOT.mem` and `GOT.func`. No relocations in code; `__wasm_apply_data_relocs` fixes pointers in
  data, once, before `__wasm_call_ctors`.
- Exported data symbols are globals holding the address *relative* to `__memory_base`.

What I built with this toolchain's clang/wasm-ld 18 (linux-wasm's LLVM 18 fork, commit 30dfc8fca)
and found:

| | LLVM 18 | fixed or changed in |
|---|---|---|
| `wasm-ld -shared -mwasm64` makes a usable side module (dylink.0, GOT.mem/GOT.func as i64 globals, `__memory_base` i64, `__table_base` i64 and `__table_base32` i32 for the 32-bit table) | yes | |
| Linking against a `.so`: wasm-ld 18 records it as `NEEDED` but reads no symbols from it; *every* undefined symbol becomes an import, silently (`lld/wasm/SymbolTable.cpp`: a SharedFile is only pushed to `sharedFiles`) | missing | LLVM 19: "Report undefined symbols in -shared/-pie builds" (#75242, 22b7b84860) models `SharedFunctionSymbol`/`SharedDataSymbol` and checks signatures |
| Tags (`__cpp_exception`) exported by a shared library resolve when linking against it | no | LLVM 23: "Add support for shared tags" (#188367) |
| wasm64 table | 32-bit table (`__table_base32`) | LLVM 19: "Use 64-bit table when targeting wasm64" (#92042): table64, so the runtime's table must become a table64 too (V8's memory64 includes table64) |
| PIC/dynamic linking marked experimental (`--experimental-pic`) | yes | LLVM 23 removes the warning (#196566) |
| Default symbol visibility | **hidden** for every wasm target (`clang/lib/Driver/ToolChains/Clang.cpp`, `AddWebAssemblyTargetArgs`): a library built without `-fvisibility=default` exports nothing but its weak symbols | unchanged |
| A shared library's *weak definitions* are imported and exported (`lld/wasm/Writer.cpp` shouldImport: "When a symbol is weakly defined in a shared library we need to allow it to be overridden by another module so need to both import and export the symbol"); `-Bsymbolic` does not change that. In C++ that is every inline function and template instantiation of default visibility. The import of its own export cannot be bound before the module exists | by design | unchanged on main; see below |
| TLS: only local-exec on targets other than Emscripten (`WebAssemblyISelLowering.cpp`, `LowerGlobalTLSAddress`: "Currently only Emscripten supports dynamic linking with threads"), so a module cannot use a thread-local variable defined in another; `wasm-ld` refuses `R_WASM_MEMORY_ADDR_TLS_SLEB64` against an undefined symbol. Every C++ landing pad uses `__wasm_lpad_context`, libunwind's thread-local: the first `-shared` link of C++ with a `catch` failed on it | missing | unchanged on main; a one-line change (allow general-dynamic for our OS) |
| `__wasm_call_ctors` exported by `-shared` | not always (my second link of libfoo did not export it) | pass `--export-if-defined=__wasm_call_ctors` |
| A module's TLS: each has its own `__tls_base` global, `__wasm_init_tls`, `__tls_size`, `__tls_align`, none exported | as designed | a 40-line `dlcrt.S` per module exports four helpers |

Consequences for the toolchain (all done in `tests/wasmdyn/build.sh` by hand; `nq-clang` would do
them in a `-shared` mode):

- Libraries compile with `-fvisibility=default` (or with export macros, which Qt and KDE have).
- Every module links in `dlcrt.o` (TLS helpers) and, if it has landing pads, libunwind's one Wasm
  object (`Unwind-wasm.o`, hidden): each module then has its own `__wasm_lpad_context`, which is
  correct because it is a scratch area between a landing pad and the personality function that the
  same module's `_Unwind_CallPersonality` reads (`libunwind/src/Unwind-wasm.c`). The personality
  routine itself (`__gxx_personality_wasm0`) is libc++abi's, shared.
- Weak self-imports are bound to *trampolines*: ld.so writes a tiny module whose functions
  `call_indirect` the slot the definition will occupy, and binds the import to it. That works for
  any import whose definition is not instantiated yet (itself, or a later module in a dependency
  cycle). libfoo has 4 such imports; a Qt library would have thousands, each call paying about 1.6 ns
  more (measured below). The right fix is in the linker: bind weak definitions locally in a shared
  library (what ELF's `-Bsymbolic-functions` does, and what Qt asks for anyway with its
  `reduce_relocations` feature), keeping data interposable so typeinfo stays unique. A small
  wasm-ld patch; upstreamable.
- With LLVM 18, a link against a `.so` checks nothing; a missing symbol is found at load time. A
  link-time check against the `.so`'s exports is a small script (nq-clang already checks imports
  after linking, `nativeqt/wasm-imports.py`); LLVM 19 makes it unnecessary.

## 2. Who links

### Alternatives

- **A. The JavaScript runtime** (Emscripten's way: `src/lib/libdylink.js` `loadWebAssemblyModule`,
  `GOT` as JavaScript `WebAssembly.Global`s, `dlopen` a host call into `_dlopen_js`). Quick to write,
  and JavaScript sees instances directly. But the host cannot open a file (only the kernel can, the
  program would read it and hand it over anyway), search paths, `RTLD_*`, refcounts, `dlerror` and
  TLS would live in JavaScript, every libc's `dlfcn` would call into it, and Joel's runtime is kept
  small on purpose. Emscripten needs it because Emscripten has no kernel and no libc-side file system
  of its own in the browser; linux-wasm has both.
- **B. The kernel** (binfmt resolving symbols): no. Linux never did symbol resolution in the kernel,
  and `dlopen` is not a kernel operation.
- **C. ld.so in the process, plus a minimal host primitive** (recommended). It is how Linux works
  (and how NOMMU Linux works: FDPIC's ld.so is user space too). Policy stays in libc, where
  `dlopen` lives; the host does only what a process cannot: make a `WebAssembly.Instance`.

### The host primitive

`tests/wasmdyn/ldso/wasm_dl.h`; the runtime side is `tools/patch-runtime.py` item 26 (about 100 lines
of JavaScript). One import into programs:

```c
long __wasm_dl(long op, void *arg);   /* 0 load(desc) -> instance id, 1 count, 2 table length */

struct wasm_dl_desc {
	uint64_t bytes, len;         /* the module, in memory (kept for the process's lifetime) */
	uint64_t nimports, imports;  /* one binding per import, in import order: */
	uint64_t table_min;          /*   HOST name     the runtime's own object (memory, table, stack pointer, syscalls) */
	uint64_t place;              /*   GLOBAL t v    a new global of that type and value (GOT, __memory_base) */
	uint64_t module;             /*   SLOT n        the function in table slot n */
	...                          /*   EXPORT m name export of instance m (tags) */
};                               /*   MISSING name  a function that traps "undefined symbol: name" */
```

Instances are numbered per Worker in load order (0 is the program the kernel started); a thread
that loads the same descriptors in the same order ends up with the same numbering and the same table.
The host does not know about files, symbols, scopes or threads. It could serve another libc.

### ld.so (prototype: `tests/wasmdyn/ldso/ldso.c`, 1,280 lines of C, statically in `ld-wasm`)

`ld-wasm` is musl + libc++ + libc++abi + the dynamic linker in one static PIE that exports the
libraries' public symbols (5,094) for the modules it loads. Run as `ld-wasm PROGRAM ARGS` (as
`/lib/ld-musl-x86_64.so.1 PROGRAM` works on any musl system); with kernel support (section 7) the
kernel starts it for any program that has `NEEDED` entries.

- **Finding files:** a name with a slash as is; else `LD_LIBRARY_PATH`, the directory of the module
  that needs it, `/lib`, `/usr/lib`. (RUNPATH/`$ORIGIN` from `dylink.0` `RUNTIME_PATH` is next.)
  The file is `mmap`ed read-only and stays mapped: threads started later compile it from there (as
  the runtime does with the executable, which the kernel keeps mapped for the same reason).
- **Reading it:** `dylink.0`, types, imports, globals (init values, for data addresses), exports
  (or the `linux-wasm.dynsym` section). Its own module it reads in place, where the kernel mapped it
  (`/proc/self/stat` fields 26-27 are `mm->start_code`/`end_code` on this kernel; with kernel
  support, auxv instead).
- **Memory:** `mmap` of `MEM_INFO`'s size (zeroed; aligned as asked). The module's start function
  (`__wasm_init_memory`) copies its data segments there on the first instantiation only (an atomic
  flag in its own data); every other thread's instantiation skips it.
- **Table:** one process-wide slot allocator in ld.so (`dl_table_top`, starting at the table's
  length). A module gets `[table_base, table_base + tablesize)`; with tablify its exported functions
  are in there too. Each Worker grows its own table to `table_min` before instantiating.
- **Binding** (`instantiate()`): scope = the global scope in load order (the program, the libraries
  it needs breadth first, `RTLD_GLOBAL` loads; the C library last), then the module's own
  dependency group (for `RTLD_LOCAL`). First definition wins, weak or not, as musl's
  `find_sym`. `GOT.mem` gets `mem_base + value` of the winning data symbol (known before
  instantiation, so cycles do not matter), `GOT.func` the winning function's slot (likewise),
  a direct function import the winning function (its slot, or its export, or a trampoline if its
  module is not instantiated yet), a tag import the C library's tag (`EXPORT`). Weak undefined
  imports get 0 or a trapping stub. Strong undefined functions get a trapping stub that names the
  symbol (lazy-binding-like: a plugin with one unresolvable function still loads, as Qt's
  `RTLD_LAZY` default expects); strong undefined data fails the load.
- **Relocations:** none in code. After instantiation, the module's `__wasm_apply_data_relocs` once
  (in the loading thread: the memory is everyone's).
- **TLS:** a module's `__tls_base` is a global of *its instance*, and instances are per Worker, so
  setting it in a thread's instance gives that thread its own block. ld.so keeps, per thread, a block
  per log entry (`dl_dtv`, itself thread-local in the C library), and on each thread's
  instantiation either makes a new block (`mmap`ed, initialised by the module's `__wasm_init_tls`
  via `__dl_tls_init`) or binds the block that is there (`__dl_tls_set`): a vfork child runs on its
  parent's C library TLS, so it finds and uses its parent's blocks, which is exactly what borrowing
  the TLS means (tested: main4). Blocks are `mmap`ed, not `malloc`ed, because this runs in signal
  handlers (`__synccall`). Not yet: freeing them when a thread exits (musl's thread exit can call
  ld.so; ELF musl does not free dynamic TLS either, it allocates it at `dlopen` for all threads).
- **Constructors:** `__wasm_call_ctors` of each new module, dependencies first, after every thread
  has the module. Destructors are registered by the constructors with `__cxa_atexit` (Wasm has no
  `.fini_array`), and the C library runs them at `exit`; the function pointers are table slots,
  valid in every thread.
- **dlopen/dlsym/dlclose:** `dlopen` serialises on a lock, first catches its own thread up with the
  log, loads the new modules (breadth first, instantiated dependencies first), makes every thread load
  them (`__synccall`), then runs constructors. `RTLD_NOW` and `RTLD_LAZY` both bind everything at
  load (Wasm imports must be bound at instantiation) except that unresolved functions trap when
  called; `RTLD_GLOBAL` puts the group in the global scope (also later, on a second `dlopen` of the
  same file); `RTLD_NODELETE` is always true; `dlclose` checks the handle and unloads nothing (musl's
  own behaviour: musl's `dlclose` never unloads). `dlsym` returns a table slot for a function and an
  address for data; `RTLD_DEFAULT` searches the global scope; `RTLD_NEXT` is not done yet (needs the
  caller's module from its return address, which Wasm does not give: an explicit
  `__dlsym(…, caller)` from a per-module stub would). `dladdr` finds the module of a slot or of a
  data address. Errors go through musl's `__dl_seterr`.
- **Symbols in a custom section (`--dynsym`):** see [Performance](#6-performance); ld.so reads
  `linux-wasm.dynsym` (the export section's encoding) when present and binds function imports to
  table slots, so libraries need no wasm exports at all.

## 3. Threads

**Emscripten** (`system/lib/libc/dynlink.c`, `src/lib/libpthread.js`): every `dlopen` and every
`dlsym` (which may add a function to the table) is appended to a linked list of events; each thread
keeps a thread-local cursor. `dlopen` on the main thread proxies `_emscripten_dlsync_self` to every
thread and blocks until all have loaded the module (`_emscripten_dlsync_threads`); background threads
also catch up in `futex_wait` (`_emscripten_process_dlopen_queue`, "whenever they block"); a new
thread gets the compiled modules in its start message (`sharedModules`) and runs
`__emscripten_dlsync_self()` before its entry point ("Otherwise our table might be not be in sync
and might not contain the function pointer"). A sync failure aborts (`ABORT_ON_SYNC_FAILURE`).
TLS: `_emscripten_tls_init` per module per thread, and `GOT.mem` entries of TLS exports rewritten per
thread (`registerTLSInit`).

**What fits linux-wasm:** the same log, kept by ld.so in shared memory, but each thread's position
is the *host's* count of instances in its Worker (`__wasm_dl(1)`), not a thread-local variable (a
vfork child borrows its parent's TLS, but not its parent's Worker). Two triggers:

1. *A new task* (thread, vfork child, `clone` with a function): before anything of the task's own
   runs, it replays the whole log. In the prototype the runtime calls the program's exported
   `__dl_clone_hook` right before `__libc_clone_callback` (patch-runtime.py item 26); in musl proper,
   `__libc_clone_callback` (musl's wasm `clone.S`) would call `__dl_sync` itself, and the runtime
   would need no hook.
2. *Threads that exist when `dlopen` loads something:* `dlopen` calls musl's `__synccall`
   (`src/thread/synccall.c`), which sends `SIGSYNCCALL` to every thread, and in each one's handler
   runs the replay. musl uses it for `setuid` and friends; it is in every musl. A thread asleep in
   the kernel (a futex wait, a `read`) takes the signal at once and replays in its own Worker on its
   way out; measured 0.08 ms with no other thread, 6 ms when the other thread was still starting.
   Because the replay happens inside the handler, nothing in it may take locks a thread could hold:
   TLS blocks are `mmap`ed for that reason, and `__wasm_dl` only touches JavaScript objects.

The kernel alternative, for upstream discussion: a per-mm "code generation" number, bumped by a
call that means "code changed" (`membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)` has exactly
that meaning on other architectures: every thread executes a core-serialising instruction before it
runs user code again), with the return-to-user path (`wasm_user_mode_tail`) calling the program's
sync hook when the thread's number is behind. Same effect without signal semantics (masks, `EINTR`
in other threads); more kernel code. `__synccall` is enough to start with.

What neither can do: reach a thread that never enters the kernel. A Worker running Wasm cannot be
interrupted by anyone, so `dlopen` waits until every running thread's next system call (Emscripten
has the same limit: a thread that never blocks never syncs). A compute loop without system calls
delays `dlopen` for as long as it runs. That is rare in Qt programs (worker threads sleep in futexes);
if it bites, `dlopen` could stop waiting for running threads after a bound and let them catch up at
their next system call, at the risk that such a thread reaches a pointer into the new library through
memory alone, before its next system call, and traps.

Not covered: a raw `clone(CLONE_VM)` child that is not a pthread (not in musl's thread list) and is
still running when another thread `dlopen`s. Today such children are vfork-style (the parent is
suspended); a long-lived one would need the kernel variant.

One ordering detail in the runtime: a signal the kernel delivered to a new task before it first ran
(patch-runtime item 22) runs its handler *before* `__libc_clone_callback`, so before the clone hook;
a handler in a library the task has not loaded yet would trap. The prototype's thread test passes
through exactly this path (the `SIGSYNCCALL` reached the new thread first, and its handler, in the C
library, did the replay), but the runtime should run the hook first.

## 4. fork, exec, vfork; sharing compiled code

- No `fork` on NOMMU. `vfork`/`clone(CLONE_VM)` children share memory, so the libraries' data is
  theirs already; their Worker replays the log (instantiations only; constructors are not run again)
  and binds the parent's TLS blocks. Tested (main4): the child calls `foo_add` and `foo_tls_bump`
  of a `dlopen`ed library, and the parent sees its own TLS variable moved by the child.
- `exec` replaces everything: the runtime starts over with a fresh instance list; the new image's
  ld.so loads what it needs. The old mappings go with the old mm.
- **Compiled code is NOT shared by V8 for identical bytes** (this was assumed here at first, from reading
  `NativeModuleCache`; measured 2026-10-12, [section 10](#10-the-host-in-the-runtime-and-the-per-page-module-cache):
  four Workers compiling the same bytes compile it four times, on every API). Only one `WebAssembly.Module`
  object posted to the Workers shares one copy of the code, so the runtime keeps one per library per page and
  starts every task with them. With static programs, each program carries (and compiles) its own copy of Qt;
  with shared libraries and the cache, one copy of each library per page, which is what matters for V8's code
  space limit (the "Aw, Snap!" at 4 GiB of committed wasm code, `tools/patch-runtime.py` item 16).
- What is *not* shared: each Worker copies the bytes out of shared memory and decodes the module
  (the cache lookup comes after decoding: a module with 20,000 imports took 18 to 28 ms per Worker
  even when cached), and instantiates it. Handing compiled `WebAssembly.Module` objects to new
  Workers by `postMessage` (Emscripten's `sharedModules`) would remove the decoding; the runtime's
  main thread would keep them per process.

## 5. Exceptions, setjmp/longjmp, RTTI

- **Tags:** a `catch` matches by tag identity, so every module in a Worker must use the same
  `WebAssembly.Tag` object. The C library (ld-wasm) defines `__cpp_exception` and `__c_longjmp`
  (`nativeqt/crt/cpp_exception.S`, `sjlj_tag.S`) and exports them; every other module imports them
  (wasm-ld makes the import by itself for an undefined tag) and ld.so binds them to the C library's
  (`EXPORT 0`). Threads never exchange exceptions, so a tag per Worker is right. Tested: thrown in
  the library and caught in the program and the other way round.
- **Landing pads:** per-module `__wasm_lpad_context` (section 1). Correct as long as the personality
  call for a landing pad goes through the same module's `_Unwind_CallPersonality`, which it does
  (the landing pad calls it directly, and it is hidden).
- **setjmp/longjmp:** Wasm SjLj (`-mllvm -wasm-enable-sjlj`) throws `__c_longjmp`; with the tag
  shared, a `longjmp` in one module to a `setjmp` in another works the same way (not yet tested in
  the prototype; FreeType inside a library longjmps within itself).
- **RTTI:** libc++ compares `type_info` by address by default
  (`_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION` 1, `libcxx/include/typeinfo`). Uniqueness comes
  from ELF-style binding: every reference to a default-visibility typeinfo in a shared library goes
  through `GOT.mem` (wasm-ld uses the GOT for any preemptible symbol in `-shared`), and ld.so binds
  it to the first definition in the global scope. Tested: `dynamic_cast` in the program on the
  library's class and in the library on the program's class, catch by base type across modules.
  The hole is the same as ELF's without `-rdynamic`: a typeinfo *defined in the program* and not
  exported (programs are built hidden) is not seen by libraries that have their own weak copy. ELF
  linkers export such symbols from the executable when a shared library on the command line
  references them; wasm-ld 18 does not read `.so` symbols, so either export `_ZTI*`/`_ZTS*` from
  programs, or build libc++ with `LIBCXX_TYPEINFO_COMPARISON_IMPLEMENTATION=2` (compare names; a
  CMake option, `libcxx/CMakeLists.txt`) as a fallback. Qt's own casts across plugins
  (`qobject_cast`, `qt_metacast` by interface id) compare meta-objects and strings, not typeinfo.

## 6. Performance

Measured under node-machine (Node 22.14's V8, M4 Max, the Mac shared with other work: ±20 %).

**Calls** (`tests/wasmdyn/bench.c`, 20 M calls each, second round; the same code as one static
program for comparison):

| | dynamic | static |
|---|---|---|
| program → program, direct | 2.0 ns | 2.0 ns |
| program → library, direct (an import from another instance) | 2.1 ns | 2.0 ns |
| program → library, through a function pointer | 2.1 ns | 2.0 ns |
| library → library, direct | 1.3 ns | 1.3 ns |
| library → its own weak function (trampoline) | 3.0 ns | 1.4 ns |
| library → program, through a function pointer | 1.7 ns | 1.3 ns |
| data through `GOT.mem` (read and write) | 1.7 ns | 1.7 ns |

A call into another module costs what a call inside one does: V8 calls an imported Wasm function of
another instance directly. Only trampolines cost (+1.6 ns per call), which is why the linker should
bind weak definitions locally.

**Starting a program** (`timeit`: posix_spawn and wait, 20 times): a static hello 4.6 ms mean (1.9
fastest), the same hello through ld-wasm 10.2 ms (9.1 fastest), main1 with libfoo 10.7 ms (10.4).
ld-wasm itself takes 1.7 to 2 ms from its `main` to reading its own 2.3 MB module and its 5,094
symbols, then about 1 ms per small module. The rest is the runtime instantiating ld-wasm's 2.3 MB (libc++ is in it
whole, `--whole-archive`) and libc++'s static initialisation; a libc.so without libc++ in it (libc++
as its own library) would start faster.

**A big library** (`libbig.so`: 20,000 exported functions, 740 KB, about what libQt6Gui exports):

| | dlopen | per new thread: decode (cached) | instantiate | put exports in the table | thread start+join |
|---|---|---|---|---|---|
| (no library loaded) | | | | | 2.6 ms |
| exports placed by the host from JavaScript | 17-18 ms | 0.8 ms | 8 ms | 5.2 ms | 16.5 ms |
| tablify (own element segment), still exported | 13-15 ms | 1.0 ms | 9 ms | 0 | 14 ms |
| tablify --dynsym (no wasm exports) | 5.2-5.7 ms | 0.5-1.0 ms | 2.3 ms | 0 | 4.5 ms |
| (not exported at all, an array of all their addresses) | 10 ms | 0.5 ms | 0.9 ms | 0 | 6.7 ms (busier run) |

V8 makes a JavaScript function object for every exported function of every instance; 20,000
exports cost about 9 ms per instantiation, in every Worker. Hence the recommendation to keep
symbols in a custom section and the functions in the table by the module's own element segment.

**Imports cost per Worker too** (`libbigcall.so`: 20,000 function imports from libbig): decoding
18-28 ms in every Worker (V8 decodes the module before it looks in its cache), instantiation 5-12 ms
(a JavaScript function object per import, `table.get`); `dlopen` 29 ms, a thread start 22-23 ms
instead of 2.6 (about 1 µs per import per thread). Qt and KDE libraries import a lot from each
other: the sum over the libraries plasmashell would load is perhaps 100,000-200,000 function imports
(to be counted on the real build), which at these rates is on the order of 100-300 ms of work for
every new thread, in that thread, before it runs. Mitigations, in order of payoff:

1. Pass compiled modules to new Workers (no decoding): the runtime keeps each process's
   `WebAssembly.Module`s and posts them with the task's start.
2. Keep Workers of a process for its next thread (a per-process spare pool whose Workers already
   have the libraries instantiated; the runtime has spare runners already, patch-runtime item 2).
3. Fewer imports: `-fvisibility=hidden` with export macros (Qt, KDE do this) and local binding of
   weak definitions; for the rest, calls through `GOT.func` + `call_indirect` instead of imports
   (an LLVM option, like `-fno-plt`; about +0.5-1 ns per cross-library call, no per-Worker binding
   at all).
4. Lazy per-thread instantiation is not possible: a call into a module a Worker has not
   instantiated traps and cannot be resumed.

For comparison, the static plasmashell also decodes and instantiates its 122 MB module in every new
thread's Worker (and copies it out of shared memory first); that cost was not measured, and should
be, before deciding how much of the above is needed.

## 7. Plan, pieces, effort, upstreaming

| # | Piece | Where | Size | Effort |
|---|---|---|---|---|
| 1 | Host primitive `__wasm_dl`, load-log replay, per-page module cache (**done 2026-10-12**, `?dl=1`) | runtime | ~250 lines JS | done |
| 2 | Sync hook: `__libc_clone_callback` calls `__dl_sync`; runtime runs the start-signal handler after it | musl `clone.S`, runtime | ~20 lines | 0.5 day |
| 3 | ld.so in musl: `ldso/wasm/dynlink.c` from the prototype, libc.so = `ld-musl-wasm64.so.1` (a PIE exporting musl's public symbols, `dynamic.list`), `dlcrt` in `crti`-like objects, `RTLD_NEXT`, RUNPATH, TLS freeing at thread exit, `dl_iterate_phdr`-like iteration for libgcc-style users | musl port | ~1,500 lines C | 4-5 days |
| 4 | Kernel: binfmt_wasm runs an interpreter for programs that need one (a `dylink.0` subsection naming it, or `NEEDED` present → a configured path), maps both, passes the program's mapping in auxv (`AT_PHDR`-like, `AT_BASE`, `AT_ENTRY` unused) | kernel | ~150-200 lines | 1-2 days |
| 5 | Toolchain: `nq-clang -shared` (default visibility for libraries, `dlcrt.o`, `Unwind-wasm.o`, `--export-if-defined=__wasm_call_ctors`, the post-link step, link-time check against `.so` exports); libc++/libc++abi as `libc++.so`; sysroot shared | rvwasm | scripts | 3-4 days |
| 6 | wasm-ld: (a) bind weak definitions locally (`-Bsymbolic-functions`), (b) every exported function in the table + symbols in a custom section (what tablify.py does), (c) `--dynamic-linker` (the interpreter subsection) | LLVM (Joel's fork, then upstream) | ~200 lines C++ | 3-5 days incl. tests; until then tablify.py and trampolines |
| 7 | LLVM: general-dynamic TLS for PIC on non-Emscripten triples (one condition), and `GOT.mem` TLS entries per thread in ld.so | LLVM, musl | small | 1-2 days; only if a library needs another's thread-local |
| 8 | Performance: modules handed to new Workers; per-process Worker pool | runtime | ~150 lines | 2-3 days |
| 9 | Qt shared: qtbase, qtdeclarative, qtwayland, qtsvg… with `BUILD_SHARED_LIBS=ON`, `QT_FEATURE_library` on, the static-plugin machinery out; tests qwidget, qquick with their platform and image plugins loaded by `dlopen` | nativeqt | build changes | 4-6 days |
| 10 | KDE Frameworks + plasma-workspace shared: the `static-plugins.py`/`kf-static-deps.py`/rename work goes away; KPluginFactory and QML plugins load as on Linux | nativeqt | build changes | 1-2 weeks |

Total about 4-6 weeks; a Qt program loading its Wayland platform plugin with `dlopen` after pieces
1-3, 5 and the Qt part of 9 (about 2 weeks). Pieces 1-4 are what the rest stands on; 6 and 8 are
what makes it fast; 7 only if needed.

**Upstream to linux-wasm, as separate pieces** (draft for Joel: `docs/upstream/wasm-dynamic-linking-proposal.md`):

1. Runtime: the `__wasm_dl` import (generic; no libc policy in it).
2. musl port: the wasm ld.so and `libc.so` build, the clone-callback sync.
3. Kernel: binfmt_wasm interpreter support and auxv.
4. Toolchain: the `fake-llvm` wrapper's `-shared` mode; the wasm-ld and LLVM patches (also for LLVM
   upstream: they are not linux-wasm specific; WASI's shared libraries hit the same weak-symbol and
   TLS limits).
5. tool-conventions: a `dylink.0` subsection for the interpreter and one for "function exports are
   in the table" (or a symbol-table section), if the wasm-ld options go upstream.

## 8. Risks

- **Thread start cost with many large libraries** (section 6). Measured on synthetic libraries;
  must be measured on Qt. Mitigations exist and are runtime-only.
- **`dlopen` waits for every running thread's next system call.** A busy thread stalls it.
- **Exports limit:** V8's maximum number of exports was 100,000 when `docs/native-wasm.md` was
  written; V8 main now says 1,000,000 (`kV8MaxWasmExports`, `src/wasm/wasm-limits.h`). With
  `--dynsym` libraries export almost nothing anyway.
- **Table size:** every exported function of every library has a slot in every Worker's table.
  V8's maximum is 10,000,000 (`kV8MaxWasmTableSize`); Qt + KF + Plasma are a few hundred thousand.
- **LLVM 18 checks nothing at link time** (`.so` symbols): errors move to load time until LLVM 19
  or a link-time script.
- **table64 when moving to LLVM ≥ 19:** the runtime's table (and ld.so's trampolines' `i32.const`)
  must follow (64-bit tables are part of the memory64 proposal); Node 22 needs its memory64 flag.
- **Typeinfo defined only in a hidden program** (section 5).
- **`dlclose` never unloads.** Same as musl; plugins stay loaded (Qt rarely unloads).
- **rv's JIT** grows each Worker's table on its own (`__rv_jit_install`): a dynamically linked
  program using it would collide with ld.so's slot allocator. rv is static; if that changes, the
  JIT takes slots from ld.so.
- **Signal at a new task's start** runs before the clone hook (section 3), to fix in the runtime.
- **Memory per process:** every library's file stays mapped (as executables are), plus per-Worker
  JavaScript objects per instance.

## 9. The prototype

`tests/wasmdyn/` (`README.md` there says how to build and run):

- `ldso/ldso.c`, `ldso_asm.S`, `wasm_dl.h`: the dynamic linker; `ldso/dlcrt.S`: per-module TLS helpers.
- the runtime's part is `tools/patch-runtime.py` item 26 (section 10); `check-cache.sh` tests the module cache.
- `tablify.py`: the post-link step (exports into the table; `--dynsym`).
- `libfoo.c`, `libfoo_cpp.cpp`, `foo.h`: the library; `main1.cpp` (linked against it), `main2.c`
  (dlopen/dlsym), `main3.c` (threads), `main4.c` (vfork child, posix_spawn), `main5.c` (scale),
  `bench.c` (call costs), `hello.c`/`timeit.c` (start-up).
- `build.sh` (build VM), `run.sh` (Mac, node-machine; waits for any other node-machine to finish).

What it proved: everything in the summary, plus the numbers in section 6. What it does not do yet:
the kernel interpreter (programs are started as `ld-wasm PROGRAM`), `RTLD_NEXT`, RUNPATH, TLS
freeing, `GOT.mem` for thread-local symbols, setjmp across modules (expected to work, not tested),
a C++ library as its own `.so` (libc++ is in ld-wasm), and anything in the browser (the owner's
checks).

## 10. The host in the runtime, and the per-page module cache

*Status 2026-10-12.* The prototype's host side is `tools/patch-runtime.py` item 26 (the separate
`runtime-dl.py` is gone). **Off unless the page has `?dl=1`** (`--query "?dl=1"` under node-machine): then
programs are given the `__wasm_dl` import and a new task calls its program's `__dl_clone_hook` before its
own function; off, no program gets either and none of this code is reached (checked: `check-cache.sh`, a
static program and the shell run as before with `?dl=0`).

### Does V8 already share compiled code? Measured: no

Node 22.14 (V8 12.4), `--trace-wasm-compilation-times` counting the functions compiled when four Workers
each take the same module (20,000 functions; a wasm32 module and `libbig.so`, `--no-liftoff` so the count is
complete; the modules kept alive in the Workers):

| how each Worker gets the module | function compilations (4 Workers) |
|---|---|
| `new WebAssembly.Module(bytes)` with identical bytes | 80,000 (4 times) |
| `await WebAssembly.compile(bytes)` | 80,000 |
| `WebAssembly.compileStreaming(Response)` | 80,000 (also with `--no-wasm-native-module-cache-enabled`) |
| one Module compiled in the main thread, posted to each Worker | 20,000 (once) |

So the engine-wide `NativeModuleCache` keyed by wire bytes (section 4 assumed it) does **not** deduplicate
across Workers here, on any compile path: identical bytes compiled in n Workers are compiled n times, and
n copies of the code are committed (what counts against the 4 GiB code limit). Only the *same
`WebAssembly.Module` object*, sent by `postMessage`, shares one copy. (RSS is a poor measure for small
modules: the 20,000 tiny functions are 1 MB of code; the compile count is the clean measure.) Chrome's V8
may differ; not measured, no test browsers. Corollary to section 4's last paragraph: sharing is something
the runtime has to do, which it does now.

### The cache

- Key: a content key of the library's bytes (`dl_key`: two position-dependent 32-bit lanes and the length,
  e.g. `0d0f9c11483ecc1e-8245`; `tools/dl-hash.py` computes the same). Computed by the Worker from the
  program's memory on every load (1-2 ms per 6 MB), not supplied by the program, so a process cannot put a
  module of its own under another's key. Not cryptographic: a cache key for one page.
- The main thread holds one `WebAssembly.Module` per key for the life of the page (so it is never
  recompiled, even after every process that used it has gone), and starts every task (`init`, and `assign`
  for the spare runners) with all of them. A Worker that has the module only instantiates it; one that does
  not compiles it (a copy of the bytes: a module cannot be compiled from shared memory) and posts it to the
  main thread (`dl_load`). `os.dl.stats()` says per key how many Workers compiled it, took it from the page's, or
  had it compiled ahead.
- Proof (`tests/wasmdyn/check-cache.sh`, 11 checks): `main1`, `main2`, `main4` and `main3` in a row, with their
  threads, vfork children and `posix_spawn` children: libfoo.so is compiled **once**, taken from the page's by 7
  loads. The program modules themselves go through the same cache (the program `ld-wasm` loads is a module too).
- **The one exception:** a thread that already exists when another thread `dlopen`s replays the load in a
  signal handler, inside a blocked system call, and its Worker cannot receive a message there (no synchronous
  receive in a browser; `receiveMessageOnPort` is Node's). It compiles the module itself (`main3` alone: 2
  compiles, the thread started after dlopen shares). Fixing that needs the kernel route of section 3
  or a main-thread proxy; libraries that are `NEEDED` load before any thread exists, so it affects `dlopen`ed
  plugins only. The cost per such thread is the module's full compile (V8 here does not skip it).
- Not cached: the program image the kernel starts (`wasm_load_executable`, still a copy and compile per exec
  and per thread: the static plasmashell's 141 MB). With ld.so as the kernel's interpreter for every
  dynamic program that image is ld-wasm, 2.3 MB, and the programs go through `__wasm_dl`.

### Code cache across visits

`WebAssembly.compileStreaming(fetch(url))` is the route the browser keeps compiled code for (Chrome's code cache
sits with the HTTP cache, keyed by URL); a Module cannot be stored in IndexedDB any more (removed from
Chrome), so storing one ourselves is not possible. `os.dl.preload([{key, url}])` compiles libraries that
way at page load, before any program asks, and puts them in the cache (`{key, bytes}` for tests; node-machine's
`--dl-preload` and `check-cache.sh`: preloaded libfoo.so, no Worker compiles it). What the page needs is a
manifest of `{key, url}` for the libraries in the disk image (generated with `tools/dl-hash.py` when the image
is built) with the libraries also served as plain, cacheable files. Not tested in a browser: the effect on
the second visit has to be measured there.

### QtCore through the loader (2026-10-12)

`tests/wasmdyn/qtcore-hello` (`build.sh` in the VM; part of `check-cache.sh`, all pass): `QCoreApplication`,
`QString`/`QStringList` (arg, toUpper, split, sort, join, UTF-8), a `QTimer` whose `timeout` signal fires a moc'd
slot, a `QThread` whose queued signal is delivered on the main thread, `quit`. The program is a PIE linked with
`libQt6Core.so.6` as NEEDED (`nqshared`'s 7,446,184-byte, 2,061-import, 7,075-export build), the C library and libc++
are `ld-wasm`'s. **It ran on the first attempt, no change to Qt, the loader or the runtime** (moc from the host
Qt as for any program). Expected output and cache: `libQt6Core.so` is compiled **once**
(`compile 1, shared 1`: the main thread compiles, the QThread takes the page's), the program and
`(trampolines)` once each.

Measured (node-machine, Node 22, one instance, warm; V8's compile of a 7 MB module is lazy/baseline here, a
browser's will differ and is not measured):

| | dynamic (`ld-wasm ./qtcore-hello` + libQt6Core.so) | static (same source, nq-clang++, libQt6Core.a) |
|---|---|---|
| program wasm | 17,135 B (+ ld-wasm 2.35 MB + libQt6Core.so 7.45 MB, both shared by every program) | 4,211,151 B (the program alone, `--gc-sections`) |
| start to `main` (cold, `LD_DEBUG=1`) | 38.6 ms: ld-wasm up 2 ms, open 2.5 ms, trampolines 3 ms, **libQt6Core compile 26 ms** + instantiate 1.9 ms, program 0.8 ms | |
| whole run (`timeit 10`, spawn to exit, 2 timer ticks + a thread) | mean 81.8 ms | mean 48.8 ms |
| QThread start to its signal delivered | 88 ms | 67 ms |

So the first Qt program costs about 33-38 ms more than a static one and every further thread about 20 ms.
Per thread (`new thread: loaded 3 modules in 19.7 ms`): **binding the 2,061 imports and instantiating costs
1.4 ms** (trampolines 0.4 ms, program 0.4 ms); the module is taken from the page's, but **14.6 ms of the 19.7 are the
content key**, `dl_key` hashing 7.4 MB in JavaScript on every load. That is the cost to remove: a thread of the
same process can be handed its process's first key by `ld-wasm` (same trust domain, the key is only a lookup for
modules the process itself loaded), after which a thread start is about 5 ms. The 4.2 MB static program size is
per program (every Qt program carries its Qt); with Gui/Widgets the shared libraries pay off in the second
program, not the first.

Next steps, in order:
1. Key handed down to threads (above), and a manifest + `compileStreaming` preload so a browser compiles
   `libQt6Core.so` before any program asks (section "Code cache across visits"); measure both in a browser run by the owner.
2. QtGui, then QtWidgets, the same way (`shared/build-qt-shared.sh` with the targets added; each needs
   `libQt6Core.so.6` NEEDED and its `-DQT_...` exports as plain default visibility). Expect the weak
   self-imports to grow (QtGui is ~3x QtCore's exports); check with `check-dylib.py --provider ld-wasm,libQt6Core.so.6`.
   The platform plugin (`wayland`/`offscreen`) and image-format plugins become `dlopen`ed modules: `dlopen` after
   threads exist hits the one exception above (a thread older than the `dlopen` compiles the plugin itself), so link
   the platform plugin as NEEDED first and keep only image/style plugins `dlopen`ed.
3. Qt's own `dlopen` of plugins relies on `QLibrary` -> `dlsym` -> `__dlsym_time64` (provided by ld-wasm now);
   verify one plugin (`QImageReader` with libqjpeg.so) before QtWidgets.
4. KCalc: KF6 libraries (KConfig, KCoreAddons, KI18n, KWidgetsAddons, KXmlGui... about 20) as shared objects in the
   same `shared/` build, `kcalc` linked as a PIE with them NEEDED; its static form is the size the plasmashell
   problem started from. Only after QtWidgets runs a `QPushButton` window on the Wayland server.

### Keys handed to threads, the manifest, and why nothing can reach a running thread (2026-10-12)

**Key handoff.** The first load of a module in a process hashes its bytes and writes the key into the load log's
descriptor (`wasm_dl_desc.key`, 32 bytes); a thread (or vfork child) replaying the log reads the key there and looks
the module up, no hashing. A key that names nothing the page has is re-checked against the bytes before anything is
compiled for it. (Every program shares one linear memory and one kernel here, so a program that lies about a key
can do nothing it could not do anyway.) Measured with qtcore-hello, `LD_DEBUG=1`: **new thread: 21-22 ms -> 5-7 ms**
(`loaded 3 modules`; 3 ms of that is building the 2,061 import bindings, 1 ms instantiating). The first load in a
process still hashes (24 ms for libQt6Core.so, 15 of them the hash); the next lever there is the hash in wasm (about
4x faster) or a file-identity shortcut, not done.

**Manifest and preload.** `tools/dl-manifest.py --out DIR LIB... [--preload NAME]` writes `DIR/libs.json` and each
library as `DIR/<key>.wasm` (the key is `tools/dl-hash.py`'s, i.e. what the runtime computes, so a preloaded
module is the one a program's load finds; `check-cache.sh` asserts it). `os.dl.preload_manifest(url)` fetches
the manifest, resolves urls against it, and compiles the libraries not marked `"preload": false` together with
`WebAssembly.compileStreaming(fetch(url))`; `dev.html` calls it with `?dl=1` for `dl/libs.json` (and says in the
log how many); `stage.sh` takes `$RVWASM_WORK/dl/` into `www/dl/` and `pack-www.sh` into the package. Under
node-machine: `--dl-manifest URL`, tested over HTTP (`check-cache.sh`: libQt6Core.so streamed once before the
program, 0 Worker compiles, the program's two loads take it). The server must send `Content-Type: application/wasm`
(required by `compileStreaming`) and, for the browser to keep compiled code, a cacheable response:
`Cache-Control: public, max-age=31536000, immutable` (the names are content keys). Mark only what most programs use
`preload: true`: a compiled library costs code space whether used or not.

**A running thread cannot be handed a module (item 3: no way found).** A thread's Worker takes a
`WebAssembly.Module` only from a message, and a Worker busy in wasm or blocked in `Atomics.wait` never turns its
event loop; a browser has no synchronous receive (Node's `receiveMessageOnPort` has no counterpart), a
`SharedArrayBuffer` cannot hold a Module, and there is no public API to serialize one. A main-thread proxy could
only post, and the thread could not read it. (Suspending the thread on JSPI instead of `Atomics.wait` would let it
return to the event loop: a different, large change.) What does work: **a thread has every module the page had
compiled when it started** (init/assign message). So a plugin that is in the manifest is compiled before the first
program, and a `dlopen` of it after threads exist is a cache hit in all of them (`check-cache.sh`, "preloaded
libfoo": `main3`, whose thread predates its `dlopen`, compiles nothing). The compile fallback remains only for a
module the page has never seen: not in the manifest, and first loaded after a thread started.

### Owner's browser test (Chrome, `?dl=1`)

Nothing here was run in a browser. What to measure: the compile time of the preloaded library on a first visit against
a second (the browser's code cache), and what a tab holds.

1. `tests/wasmdyn/run.sh true` (fetches the build VM's `out/` and builds the runtime), `tests/wasmdyn/browser-prepare.sh`,
   `tests/wasmdyn/browser-serve.py build/wasmdyn/browser` (port 8123; the dev server on :8000 must run: it passes
   everything else through, and serves `dl/*.wasm` as `application/wasm`, immutable, with the isolation headers).
2. Open `http://localhost:8123/dev.html?image=wasmdyn&dl=1` in a fresh Chrome profile window (DevTools closed until
   booted; no extensions). The console log says `[Dl]: 1 libraries compiled ahead`. In the DevTools console:
   `linuxOs.dl.log.filter(e => e.how == "preload")` - `us` is the compile time of libQt6Core.so (7.4 MB) in
   microseconds, `linuxOs.dl.stats()` the rest. Note it. Then type in the machine: `cd /wasmdyn && LD_DEBUG=1 ./ld-wasm ./qtcore-hello`:
   expect `done rc=0`, and `new thread: loaded 3 modules in ~5 ms` (Node: 5-7).
3. **Second visit:** close the tab, open the same URL again (normal load, not a hard reload: that bypasses the cache);
   repeat the console command. Do it a third time (Chrome writes a module's code to the cache after it is used
   and settled, so the saving may show on the third load, not the second). In DevTools > Network the `.wasm`
   should say `(disk cache)`. **The number to report: `us` on visits 1, 2, 3.** (Cold compile of 7.4 MB measured
   under Node: 26 ms for the baseline compile; Chrome's tiering differs.)
4. **Memory:** Chrome's task manager (Shift+Esc, "Memory footprint" and "JavaScript memory" for the tab) after boot, and
   after running qtcore-hello three times, in two runs: with the manifest (above), and without
   (`?image=wasmdyn&dl=1&nodl` is not a thing: delete `build/wasmdyn/browser/dl/libs.json` instead - the page then logs
   `no libraries compiled ahead` and each program's first load compiles). In the console,
   `await performance.measureUserAgentSpecificMemory()` (the page is cross-origin isolated) gives a breakdown. Also
   report whether `linuxOs.dl.stats()` shows `compile: 1` or more for the Qt key in the no-manifest run (in Chrome the
   threads' Workers must take the page's module: `shared`), which is the claim that V8 holds the code once.

### QtWidgets on the window server (2026-10-12)

`tests/wasmdyn/qtwidgets-hello` (a `QMainWindow` with a `QLabel`, a `QPushButton` and a moc'd slot; PIE, 21,675 bytes,
`libQt6Widgets/Gui/Core.so.6` NEEDED) runs against branch `nqshared`'s shared Qt, under node-machine with `?dl=1`, on
wlserver (the Debian disk's `desktop wayland`), and tools/wl-dump.js's screenshot shows the window:
`docs/img/wasmdyn-qtwidgets-hello.png` ("clicked 1 time": the program clicked its own button, the slot changed the label).
`tests/wasmdyn/run-widgets.sh` builds the machine's overlay (libraries and their versioned names as symlinks in `/usr/lib`,
plugins in `/usr/lib/qt6/plugins`, `/usr/share/X11`, `/usr/share/fonts` and `/etc/fonts` linked into the Debian disk's)
and `check-widgets.sh` asserts it (9 checks pass; `check-cache.sh` still 22).

What it took, in order:
1. **The loader loaded a module twice** when two names or two paths of a diamond reached it: `libQt6Core.so.6` (the program's
   NEEDED) and `libQt6Core.so.6.8.2` (Gui's), and Widgets and Gui both needing Core in one load (the second `open_dso` ran
   before the first was in the lists). `open_dso` now identifies a file by (dev, inode) and keeps a list of every module
   opened (`dl_all`), loaded or not. Two Cores would have been two QCoreApplications.
2. **`xkb_context_new` failed** (no `/usr/share/X11/xkb`, outside the Debian chroot) and libxkbcommon asserts on the NULL
   it then gets (`atom_intern`): not the shared build (`xkbtest`, a keymap compiled through `libxkbcommon.so`, passes).
3. **fontconfig had no cache directory** (`HOME`, `XDG_CACHE_HOME`): 1.4 s in `QApplication`; with one, 0.2 s, and the first
   window costs 3.5 s the first time in a machine (fontconfig scans the fonts) and 0.1 s the second.

Measured (node-machine, Node 22, the Mac and the build VM busy with other work, so +-30 %):

| | |
|---|---|
| libraries loaded for the program | 10: Core 7.4, Gui 15.1, Widgets 8.6, DBus 2.3, WaylandClient 1.4, xkbcommon 0.95, wayland-client 0.58 MB, platform plugin, xdg-shell, bradient (decoration) = 34.8 MiB, shared by every Qt program; the program 21.7 KB. The static `qwidget-test` (label, canvas, button) is 40.2 MB, per program. |
| process start to `QApplication` constructed | 224-324 ms (first run in a machine), 87 ms (second) |
| `QApplication` to window shown | 3.2 s first run (fontconfig scanning), 13 ms second |
| process mappings at the end (`/proc/self/maps`) | 63.6 MiB (35 of it the libraries' bytes, kept mapped for threads to compile) |
| new thread (13-14 modules, 7 libraries with thousands of imports each) | 44-56 ms with the page's modules (the program started three: Qt's own) |
| compiles by a Worker, libraries preloaded through the manifest | **0** (10 compileStreaming'd ahead, 4 loads each take the page's) |
| compiles by a Worker, no manifest, two program runs in one machine | **1** per library (the second run: 0); `libxdg-shell.so` **2** |

The last row is the dlopen-after-threads gap showing: Qt `dlopen`s xdg-shell after its threads exist, and without the
manifest those threads compile it themselves; with it (`preload: true`) it is in their start message and nothing compiles.
Libraries a program may `dlopen` belong in the manifest.

**The 22 `wl_*_interface` symbols** (`dup-syms.py`): defined by both `libQt6WaylandClient.so` and `libwayland-client.so`
(same bytes), the only duplicates of that kind (59 symbols are defined in more than one module in all, 51 of them data, the
other 37 across Core, DBus, Gui, Widgets and the plugins: Qt's weak template statics). Binding is first definition in the
scope of the binding module (the global scope, then its own `dlopen` group), as ELF; a module's references all go through
`GOT.mem`, so each scope sees one copy. Two `dlopen` groups can have different first definitions, but the copies are equal and
libwayland compares no interface pointers; the window, the shell integration and the click work. If it ever matters, give
`libwayland-client.so` a place before WaylandClient in the program's NEEDED order.

### KCalc on the shared KF6 (2026-10-12)

`tests/wasmdyn/run-kcalc.sh` (closure of NEEDED entries by `closure.py`; overlay of 30 library names, 6 plugins incl. the
`kiconthemes6` icon engine, qsvg, qjpeg; manifest of every real file) runs KCalc 24.12.3 (`nqshared`'s `sysroot-shared/usr/bin/kcalc`, a
1.16 MB dylink main module, no data files installed) under node-machine with `?dl=1` on wlserver. **It started on the first
attempt with the 25 libraries and the plugins in the overlay, nothing changed in Qt, KF6, the loader or the runtime.** The window
(`docs/img/wasmdyn-kcalc.png`, menu, display, keypad, `NORM`; no icons or title text: the theme and the data files are not in the
image) takes its input from wl-dump: clicking `2` `+` `3` `=` shows `5` (`wasmdyn-kcalc-2plus3.png`), typing `*4` Enter shows `20`
(`wasmdyn-kcalc-times4.png`).

Measured (node-machine, Node 22, the Mac at load 7 on the run reported; two earlier runs at load 30+ took 5-15 times longer in
every phase, so read the times as +-50 %; first run in a machine, so fontconfig scans the fonts):

| | |
|---|---|
| modules loaded by the program | 50 (35 libraries and plugins, 53.0 MiB, shared by every Qt/KF6 program; the program 1.16 MB; 16 trampoline modules) |
| process start to `main` (`LD_DEBUG`) | 297 ms (opening, binding and instantiating 50 modules) |
| process start to window mapped | about 5.6 s (3.5 s of it fontconfig's first scan; second window in a machine: 13 ms for QtWidgets-hello) |
| new thread (47-50 modules, 4 threads in all) | 137-164 ms with the page's modules (QtWidgets-hello: 44-56 ms, 14 modules) |
| process mappings at the end (`/proc/PID/maps`) | 96.9 MiB in 781 mappings (53 MiB of them the libraries' bytes, kept for threads) |
| compiles by a Worker, manifest preloaded | **0 of 35 libraries**: each compiled once by `compileStreaming` (2.5 s summed, run in parallel), 232 loads took the page's module |
| other compiles (once each) | `kcalc` itself (1.16 MB, 3 loads shared) and 17 trampoline modules of 85 B - 19 KB |

So the claim "every library compiled once" holds for 35 of 35 under the manifest; the program and the generated trampolines
are compiled once each as well (the trampoline modules are per-process and tiny). Without the manifest the libraries are compiled
once per library by the first Worker that needs it, as for QtWidgets (dlopen-after-threads gap excepted: the KIconEngine,
qsvg and qjpeg plugins are in the manifest for that reason). Thread start grows with the module count (about 3 ms per module at 1,300
imports per module on average): KCalc's threads are Qt's own, so it is background latency; the `-Bsymbolic-functions` item above is
the lever.

Owner's Chrome steps: `the manual test sheets` (`KCALC=1 tests/wasmdyn/browser-prepare.sh`, then browser-serve.py).

### Thread start: where the 45-70 ms go, and what would cut it (2026-10-12)

Measured per new Worker of qtwidgets-hello (14 modules, 17,900 imports; node-machine, instrumenting `__wasm_dl`; the Mac
loaded, so the absolute numbers are +-30 %): `WebAssembly.Module.imports()` about 10 ms, building the import objects about
20 ms (`table.get` for each function slot, a `WebAssembly.Global` for each GOT entry, one JS property each), `new
WebAssembly.Instance` about 40 ms (about 1 ms per module however small, plus about 1 us per import; the five 1-import
trampoline modules cost 1-3 ms each). Reading the plan through typed arrays instead of per-word `BigInt`s (tried, reverted)
moved the binding by less than the noise: it is V8's creation of the objects, not the plan walk. So **(b), caching resolved
imports, is not a lever** (the objects are per Worker and cannot be shared), and **(c), lazy binding, does not reduce
instantiation** (every import is still provided then).

What dominates is the number of imports, and 85-90 % of those are modules importing their own weak definitions
(`impstat`: Widgets 6,436 imports, 2,987 of them `GOT.func` and 758 `GOT.mem` of its own symbols; Gui 4,155: 1,700 and 1,008;
Core 2,061: 932 and 487 plus 344 functions). In order of payoff for risk:
1. **wasm-ld `-Bsymbolic-functions`** (design item 6a: `requiresGOTAccess` already skips the GOT for a defined symbol
   under `-Bsymbolic`; what is missing is a functions-only variant and not importing weak function definitions in
   `shouldImport`; about 15 lines in lld/wasm, data stays interposable so typeinfo stays unique). It removes the
   `GOT.func` self-imports and the function trampolines: Widgets 6,436 -> about 3,450, Gui 4,155 -> about 2,300, and the
   trampoline modules; roughly 40-45 % of the thread start (45-70 -> about 25-40 ms). Needs the linux-wasm LLVM fork rebuilt
   (lld only) and the libraries relinked; not done here (owner of the toolchain: nqshared).
2. **A warm pool per process** (idea (a)): a spare Worker that has replayed the process's load log ahead of the clone (its
   instances bound to its own table; `__stack_pointer` set and TLS blocks bound at assignment). Gets a thread to a few ms
   when a spare is ready, but the runtime's spare runners are for kernel tasks (vmlinux instantiated, the program loaded at
   assignment) and a pre-instantiated program does not fit that flow without reworking `user_executable_setup`; Qt starts
   its three threads together at start-up, which a pool of one would not cover. Larger change, to be done after 1.
Neither alone reaches 10 ms; 1 and 2 together should. Thread start latency is off the main thread (the parent continues),
so for KCalc and Konsole it delays background work, not the window.

## References

- WebAssembly tool-conventions, `DynamicLinking.md` (dylink.0, GOT.mem/GOT.func, `__memory_base`,
  `__table_base`, `__wasm_apply_data_relocs`, implementation status).
- Emscripten: `system/lib/libc/dynlink.c` (event log, `_emscripten_dlsync_self`, proxying),
  `src/lib/libdylink.js` (`loadWebAssemblyModule`, `GOTHandler`, `updateGOT`, `relocateExports`,
  `isInternalSym`), `src/lib/libpthread.js` (`registerTLSInit`, `sharedModules`,
  `_emscripten_dlsync_threads`).
- LLVM: `lld/wasm/Writer.cpp` (`shouldImport`), `lld/wasm/Relocations.cpp` (`requiresGOTAccess`,
  `-Bsymbolic`), `lld/wasm/Driver.cpp` (synthetic PIC globals, `__table_base32`),
  `lld/wasm/SymbolTable.cpp` (SharedFile in 18), `llvm/lib/Target/WebAssembly/WebAssemblyISelLowering.cpp`
  (`LowerGlobalTLSAddress`), `llvm/lib/CodeGen/WasmEHPrepare.cpp` (`__wasm_lpad_context`),
  `clang/lib/Driver/ToolChains/Clang.cpp` (`AddWebAssemblyTargetArgs`), `libunwind/src/Unwind-wasm.c`,
  `libcxx/include/typeinfo`, `libcxx/CMakeLists.txt`, `libcxxabi/src/private_typeinfo.cpp`.
  Commits: #75242 (22b7b84860, LLVM 19), #92042 (39d32b238d, LLVM 19), #84288 (LLVM 19), #98961
  (LLVM 19), #108146 (LLVM 20), #188367 (LLVM 23), #196566 (LLVM 23); first release found with
  GitHub's compare against the `llvmorg-N.1.0` tags.
- musl 1.2.5: `src/thread/synccall.c`, `src/ldso/` (dlopen/dlsym/dlclose stubs and `__dl_seterr`).
- V8: `src/wasm/wasm-engine.h` (`NativeModuleCache`, keyed by wire bytes; one engine per process),
  `src/wasm/wasm-limits.h`.
- WebAssembly proposals list (shared-everything threads: phase 1).
- linux-wasm: `runtime/linux-worker.js`, kernel `fs/binfmt_wasm.c`, `arch/wasm/kernel/process.c`,
  musl patch (`arch/wasm/crt_arch.h`, `reloc.h`, `src/thread/wasm/clone.S`,
  `src/signal/wasm/restore.S`); this repository's `tools/patch-runtime.py`, `docs/native-wasm.md`
  (nativeqt branch).
