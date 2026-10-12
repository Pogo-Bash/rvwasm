# Issues for linux-wasm (drafts)

Found while running large native programs (a riscv64 emulator, Qt 6, KDE Plasma) on linux-wasm, wasm64,
2026-10-10. Each has been reproduced and fixed in our tree; patches below are against linux-wasm's musl port.

---

## 1. Thread stacks are 8 bytes off 16-byte alignment (wasm `_Clone`)

**Symptom.** Multi-threaded programs silently write to wrong addresses. In our case a `qsort()` running in a
second thread wrote through a null pointer, corrupting the first bytes of linear memory.

**Cause.** `pthread_create()` passes an 8-byte-aligned stack top to `__clone`, and `src/thread/wasm/clone.S`
uses it as is. Other architectures align it in their `__clone`. clang assumes a 16-byte-aligned stack and
addresses locals as `frame | 8` (e.g. the second element of a local pointer array); on a stack 8 bytes off,
that lands on the first element.

**Fix.**

```diff
--- a/src/thread/wasm/clone.S
+++ b/src/thread/wasm/clone.S
@@ -105,7 +105,11 @@ _Clone:
 		br_if 0		/* if (stack == NULL): break to block 0; */
 				/* else if (stack != NULL): */
 
+		/* Align to 16 bytes first: pthread_create() gives an 8-byte aligned top, and clang assumes 16. */
 		local.get 1
+		iLONG.const -16
+		iLONG.and
 		iLONG.const 16
 		iLONG.sub
 		local.tee 7
```

---

## 2. `__libc_handle_signal` reads the SA_SIGINFO pointer from address 12, not the signal frame

**Symptom.** SA_SIGINFO handlers never work in native programs. Worse, once anything writes to bytes
8–15 of linear memory (see issue 1), *every* signal handler in *every* program traps with
"null function or function signature mismatch", taking down init, the shell and everything else at once.

**Cause.** An extra `iLONG.const 4` in `src/signal/wasm/restore.S`: the `iLONG.add` then adds the two
constants (4 + 8 = 12) instead of adding 8 to `__stack_pointer`, so the info pointer is loaded from
address 12 instead of from the frame. While that address holds 0 the plain-handler path is taken and things appear to work.

**Fix.**

```diff
--- a/src/signal/wasm/restore.S
+++ b/src/signal/wasm/restore.S
@@ -45,7 +45,6 @@ __libc_handle_signal:
 
 	/* info_param */
 	global.get __stack_pointer
-	iLONG.const 4
 	INSN_32_64(i32.const 4, i64.const 8)
 	iLONG.add
 	iLONG.load 0
```

Test: one handler installed with `sa_handler` and one with `sa_sigaction` + `SA_SIGINFO`, `raise()` each.

---

## 3. `syscall()` with `int` arguments passes garbage (wasm64)

**Symptom.** `syscall(SYS_futex, addr, FUTEX_WAIT, val, ...)` reaches the kernel with arguments shifted
and packed together; Qt's `QMutex` waited on the wrong values and never woke.

**Cause.** musl's `syscall()` reads every variadic argument as `long` (`va_arg(ap, syscall_arg_t)`), but
WebAssembly varargs keep an `int` at 4 bytes, so mixing `int` and `long` arguments desynchronises the
reads. On other 64-bit ABIs integer varargs are promoted to 8-byte slots, which hides this.

**Possible fix.** Make the public `syscall()` cast each argument to `long` (a macro in `<unistd.h>` for
wasm), or read arguments with the caller's real types where known. We currently cast in our sysroot's
`<unistd.h>`.

---

## 4. compiler-rt long-double comparisons return 64-bit on wasm64

**Symptom.** The first `printf("%f", …)` traps or prints nothing (e.g. busybox `printf "%.1f\n" 2.5`).

**Cause.** In compiler-rt 18 the soft-float comparison helpers (`__eqtf2`, `__letf2`, …) return
`CMP_RESULT`, which is `long` (64-bit) on wasm64, while clang's caller expects `int`.

**Fix.** Build compiler-rt builtins with a 32-bit `CMP_RESULT` for wasm (or use an LLVM release where the
type matches the caller); we rebuilt the builtins with an `int` return.

---

## 5. (Design note) The 256 KiB main stack has nothing guarding what lies below it

The main thread's stack is a plain allocation; a program that overruns it (Qt's QML engine expects
8 MiB) writes into whatever the page allocator placed below it, kernel memory included. A guard (an
unmapped/poisoned region, or a canary checked at syscall entry) and a larger default (or honouring
`PT_GNU_STACK`-like metadata / `RLIMIT_STACK`) would turn silent corruption into a clean fault.
