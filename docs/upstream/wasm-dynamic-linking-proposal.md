# Proposal for linux-wasm: shared libraries and dlopen (draft, not sent)

To: Joel Severin. Draft 2026-10-11; nothing here has been posted.

---

**Subject: Shared libraries and dlopen for native programs: a design and a working prototype**

Hi Joel,

We run large native programs on linux-wasm (Qt 6 and KDE Plasma among them, built with a clang 18
toolchain for your wasm64 musl port). Today every program is one static PIE, so Plasma's shell is a
120 MB module with every plugin it might load compiled in, and every Qt program carries its own Qt.
We would like linux-wasm to have real shared libraries and `dlopen`, and we think it fits your design
with small, separable changes. We have a prototype that works on your kernel and runtime (under
Node, with our runtime patches); the full write-up with measurements is attached
(`wasm-dynamic-linking.md`). The short version:

**Format.** Nothing new: the tool conventions' `dylink.0` side modules (`wasm-ld -shared`), and the
PIE programs you already run. binfmt_wasm already insists on `dylink.0`, so programs are ready.

**The dynamic linker lives in the process, in musl.** As on any musl system, `libc.so` is the
dynamic linker. It opens the library, `mmap`s its data, assigns its function-table slots from one
process-wide allocator, looks up every import (first definition in the global scope wins, as
musl's `find_sym`), and asks the host to instantiate. The host's whole part is one import:

```c
long __wasm_dl(long op, void *arg);   /* load(desc) -> instance id; count; table length */
```

where the descriptor says where the module's bytes are and what each of its imports is bound to: one
of the runtime's own objects (memory, table, stack pointer, syscalls), a new global with a given
value (`__memory_base`, `GOT.mem`, `GOT.func`), the function in table slot N, or an export of an
instance loaded before (the C++ exception tag). About 90 lines in `linux-worker.js`; no files, no
symbols, no policy, and nothing musl-specific in it.

**Threads.** Each Worker has its own instances and table, so a load has to happen in every thread.
musl keeps a log of loads in shared memory; each thread replays what it has not loaded (same
descriptors, same order, so the same table slots in every Worker). A new task replays before its
first instruction (`__libc_clone_callback` calls the replay, or, as in our prototype, the runtime
calls an exported hook right before it); threads that exist when `dlopen` loads something are made
to replay by musl's own `__synccall` (a signal to each, whose handler replays). No kernel change is
needed for that. A kernel alternative, if you prefer it, is a per-mm code generation counter checked
on the return to user mode (the meaning of `membarrier(... SYNC_CORE)` elsewhere).

**TLS.** A module's `__tls_base` is a global of its instance, so each Worker's instance simply gets
its own block. A vfork child (which runs on its parent's TLS) finds and reuses its parent's blocks.

**Exceptions and RTTI** work across modules with the C library's tags imported by every module and
ELF-style `GOT.mem` binding (typeinfo stays unique).

**Kernel.** For `./program` to work without typing `ld.so ./program`, binfmt_wasm would run an
interpreter for a module that names one (or that has `NEEDED` entries), as binfmt_elf_fdpic does,
and pass the program's mapping in auxv. About 150-200 lines; until then
`/lib/ld-musl-wasm64.so.1 ./program` works with no kernel change at all.

**What we tested** (all on your kernel, under Node): a C++ program linked against a shared library
(calls, data, function pointers both ways, virtual calls, `dynamic_cast` either way, exceptions
thrown in one module and caught in the other, the library's TLS and constructors); `dlopen`/`dlsym`/
`dlclose`/`dladdr`; a thread that started before `dlopen` calling into the library; a vfork child
calling into a `dlopen`ed library; `posix_spawn`. Calls between modules cost what calls inside one do
(about 2 ns either way; V8 calls another instance's function directly).

**What we found in LLVM 18** that you may meet too: clang defaults to hidden visibility for every
wasm target; `wasm-ld -shared` imports *and* exports a library's own weak definitions (every C++
inline function), which cannot be bound before the module exists (we bind them through small
generated trampoline modules; a linker option to bind them locally would be better); only
local-exec TLS outside Emscripten, so modules cannot share a thread-local (libunwind's
`__wasm_lpad_context` was the first casualty; one copy of `Unwind-wasm.o` per module is correct and
avoids it); and wasm-ld 18 reads no symbols from `.so` inputs (LLVM 19 does). V8 also makes a
JavaScript function object for every exported function of every instance in every Worker, which
made a library with 20,000 exports cost 9 ms per thread start; keeping the symbols in a custom
section and the functions in the table (by the module's own element segment) brought that to about
2 ms. That is a post-link step today and could become a wasm-ld option.

**How it could go in, as separate pieces:**

1. Runtime: the `__wasm_dl` import (generic).
2. musl port: the wasm `ld.so`/`libc.so` and the replay in `__libc_clone_callback`.
3. Kernel: interpreter support in binfmt_wasm (optional; `ld.so ./prog` works without it).
4. Toolchain: a `-shared` mode in the fake-llvm wrapper; the wasm-ld/LLVM changes (which are not
   linux-wasm specific and could go to LLVM directly).

We are happy to prepare these as patches against your tree, one at a time, in whatever form suits
you, and to change the design where you see it differently: in particular, whether you would rather
have the replay triggered by the kernel than by `__synccall`, and whether the host primitive
should look different.

Thanks for linux-wasm!
