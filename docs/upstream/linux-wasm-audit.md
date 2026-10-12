# linux-wasm audit (2026-10-10)

An audit of Joel Severin's linux-wasm (runtime at 3103d5c "runtime: Unify wasm64 hack into the general wasm32
code", kernel 039e5f3e5 "wasm: hvc: Add Wasm console support", musl 5fca156 "NOMERGE: Hacks to get Linux/Wasm to
compile") for the classes of bug already found in it: wasm64 against wasm32 assumptions, alignment, signal
delivery, clone/exec/exit, futexes and the host's wait/notify, binfmt_wasm, musl's wasm arch files.

How it was done: the upstream files were read as they are in those commits (`git archive HEAD` of the trees in
the build VM; our patches in `patches/` show what we changed). Where it was cheap, a small C program shows the bug:
`tests/linux-wasm-audit/NAME.c`, built with `tests/linux-wasm-audit/build.sh NAME` (tools/wasm-cc in the VM) and
run with `tests/linux-wasm-audit/run.sh '/audit/NAME'` (tools/node-machine.js: no browser). Those runs use OUR
kernel, runtime and musl (what the VM had staged on 2026-10-10), so a reproduced bug is one our tree still has; an
upstream-only bug (fixed by our patches) is shown by reading only. Every "Output" below is what the run printed.

Status (branch `lwfix`): 1-14 are fixed in our tree (`patches/kernel-lwfix.diff`, `tools/patch-runtime.py` 20-22,
`patches/musl-wasm64.diff`), and the reproducers are regression tests (`tests/linux-wasm-audit/check.sh`); the same
fixes against pristine upstream are in `docs/upstream/patches/`.

Ranked by severity. "Ours" says whether our tree has it.

Already found and not repeated here: #19-#22 (`docs/upstream/linux-wasm-issues.md`), and, fixed in our tree but not
filed: the stale wake in `lock_wait` (patch-runtime.py 14), nested signal handlers growing the native stack (9),
kernel-mode exceptions taken as program faults (10), the 4 GiB memory cap and 32-bit host addresses (15), the
function-table import misparse and data relocations re-applied on new threads (nativeqt), musl wasm64
`__SYSCALL_LL_E` / statfs64 / `IPC_64` (`patches/musl-wasm64.diff`), TASK_SIZE 3 GiB and head.S's grow loop
(`patches/kernel-memsize.diff`). A second list near the end has bugs our patches fix that were on neither list.

---

## 1. A program that traps with a handler for SIGILL/SIGSEGV hangs for good, unkillable

- **Severity:** hang (the process never ends, its CPU stays taken, its parent waits forever).
- **Where:** `arch/wasm/kernel/traps.c:174-210` (`do_exception`, `raise_exception`) and the runtime's
  `wasm_error` (`runtime/linux-worker.js:310-322`).
- **Cause:** every Wasm trap of a user program (unreachable, out-of-bounds access, division by zero, a stack
  overflow) goes to `raise_exception()`, which does `force_sig_fault(SIGILL)` and `irqentry_exit_to_user_mode()`.
  With no handler the default action kills the task inside the kernel, and that works. With a handler (crash
  reporters do this: KCrash in every KDE program, Firefox, V8, the JVM, Python's faulthandler) the kernel sets up a
  signal frame and sets `_TIF_DELIVER_SIGNAL`, but nothing ever runs the handler: `raise_exception()` returns to
  the host, which throws `"raise_exception() returned"`, and the Worker sits idle. The task is still "running in
  user mode" as far as the kernel knows, so it never enters the kernel again: SIGKILL does not end it either (see
  2), and `wait()` in the parent never returns.
- **Repro:** `fault.c` (installs a SIGILL/SIGSEGV handler, then `__builtin_trap()`; with an argument it blocks
  SIGILL instead).
  Output: `/audit/fault x` prints `trapping`, `Illegal instruction`, `rc=132` (correct: blocked SIGILL is forced to
  the default). `/audit/fault` prints `trapping` and nothing more until the 30 s limit (expected: `handler ran`,
  status 7).
- **Proposed fix:** a fault cannot be resumed, so run the handler and then kill: in `wasm_error`, after
  `raise_exception()`, ask the kernel whether a frame was set up (`user_mode_tail()` gives 1), and if so run
  `__libc_handle_signal` exactly as `wasm_user_mode_tail` does (catching the `signal_return` Trap); then call a
  second kernel entry that ends the task with `force_fatal_sig(sig)`. The smallest fix that removes the hang (the
  handler is then not run) is `force_fatal_sig(SIGILL)` instead of `force_sig_fault()` in `do_exception()` for
  user mode.
- **Ours:** has it (our item 10 changes only the kernel-mode case).

## 2. Signals, SIGKILL included, never reach a task that makes no system calls

- **Severity:** hang (unkillable processes; `exit()` with a computing thread never finishes; `exec()` from a
  threaded process likewise). Design-level, but with consequences that look like bugs.
- **Where:** the whole delivery path: signals are only acted on in `syscall_exit_to_user_mode()` inside
  `__wasm_syscall_N` (`arch/wasm/kernel/traps.c:53`); nothing interrupts a Worker that is running user code.
- **Cause:** there is no preemption of user code. `kill -9` sets `TIF_SIGPENDING` on a task that never comes back
  into the kernel. `exit_group()` from one thread (`zap_other_threads()`) and `de_thread()` in exec wait for the
  other threads to die, which they cannot while they compute; the thread group leader stays a zombie that the
  parent cannot reap.
- **Repro:** `spin.c` (an endless loop), `spinexit.c` (main returns while a second thread computes).
  `run.sh '/audit/spin & sleep 1; kill -9 $!; sleep 2; kill -0 $! && echo STILL ALIVE after kill -9'`
  prints `STILL ALIVE after kill -9`. `/audit/spinexit; echo exited` prints `main returns` and never `exited`.
- **Proposed fix:** for fatal signals the host can do what the program cannot: when a fatal signal (SIGKILL, a
  group exit, `de_thread`) is sent to a task that is in user mode on another Worker, have the kernel call a host
  hook (`wasm_kill_user_task(task)`); the main thread terminates that Worker and starts a runner for the same task
  whose first act is a kernel entry that runs the exit path (`do_group_exit()`): the task's kernel stack is not in
  use while it is in user mode, so a fresh Worker can take it over. Non-fatal signals to computing code need
  either compiler-inserted polls (a check of a per-thread word at loop back-edges and function entries) or
  accepting that they wait for the next system call; that limit belongs in the documentation.
- **Ours:** has it. (rv's stop-the-world kick also waits for a system call.)

## 3. With SA_RESTART the handler runs only after the restarted system call has finished

- **Severity:** hang / wrong result. A handler for SIGCHLD, SIGWINCH, SIGALRM, SIGUSR1 with SA_RESTART (the default
  of `signal()` in musl, and what shells and event loops use) is held back for as long as the interrupted call
  blocks, possibly forever. musl's thread cancellation (SIGCANCEL is installed with SA_RESTART) never happens for a
  thread blocked in `read()` because of this (see 5).
- **Where:** `arch/wasm/kernel/traps.c:39-67`, the restart loop in `__wasm_syscall_N`.
- **Cause:** `syscall_exit_to_user_mode()` sets up the signal frame (`handle_signal()` leaves `-ERESTARTSYS` when
  SA_RESTART is set, `-ERESTARTNOINTR` always), and the loop then restarts the call right away, inside the kernel.
  The handler is run by the host only after `__wasm_syscall_N` has returned (`_user_mode_tail` in the asm foot of
  `entry.S`), i.e. after the restarted call is done. On other architectures the frame records a program counter
  rewound to the system call, so the restart happens after the handler.
- **Repro:** `restart.c`: SIGALRM at 1 s while `read()` waits on a pipe a thread writes at 3 s.
  Output:
  `SA_RESTART: read returned 1 at 3066 ms; handler ran at 3065 ms (LATE: the handler waited for the restarted read)`
  and, without SA_RESTART, `read returned -1 at 1002 ms; handler ran at 1002 ms (ok)`.
- **Proposed fix:** do not restart in C when a frame has been set up in this return; restart in the asm wrapper,
  after the handler, with the arguments it still has in its locals:
  ```c
  /* traps.c, in the loop of __wasm_syscall_N */
  case -ERESTARTSYS:
  case -ERESTARTNOINTR:
          /* With a handler to run first, the caller restarts the call when the handler has returned. */
          restart = !test_thread_flag(_TIF_DELIVER_SIGNAL);
          break;
  ```
  and in `entry.S` wrap each `wasm_syscall_N` body (HEAD, the call, FOOT with `_user_mode_tail`) in a `loop`
  that keeps the result in a local and branches back while it is `-ERESTARTSYS` or `-ERESTARTNOINTR` (only
  returned now when a handler was run; the handler's `rt_sigreturn` has put the stack pointer and TLS back, so
  locals 0 and 1 are still right). `-ERESTARTNOHAND` and `-ERESTART_RESTARTBLOCK` become `-EINTR` in
  `handle_signal()` when there is a handler, so they need nothing.
- **Ours:** has it (our "one handler per return" change, 2a3e2ef, does not touch the restart).

## 4. A signal pending at a new task's first return to user mode kills it with SIGILL

- **Severity:** crash (the new process or thread dies instead of running the handler).
- **Where:** `runtime/linux-worker.js:180-187` (`wasm_user_mode_tail`) with `ret_from_fork` in
  `arch/wasm/kernel/entry.S` and `vmlinux_run` / `user_executable_setup` (`linux-worker.js:380-392, 398-474`).
- **Cause:** `ret_from_fork` calls `_user_mode_tail` right after `__ret_from_fork()`. If a signal is pending
  (sent between `clone()` and the child's first run, which takes a Worker start, milliseconds), the kernel sets up
  a frame and the host is asked to run the handler, but the child's program has not been instantiated yet:
  `user_executable_instance` is null, the access throws a TypeError, `wasm_error` raises an exception, and the task
  dies with SIGILL. Ctrl-C (SIGINT to the foreground group) or a SIGCHLD/SIGWINCH arriving while a shell starts a
  child can hit this.
- **Repro:** `earlysig.c`: a SIGUSR1 handler, `clone(CLONE_VM|SIGCHLD)`, `kill(child, SIGUSR1)` at once.
  Output: `Wasm crash: TypeError: Cannot read properties of null (reading 'exports')` in the child's runner and
  `child: killed by signal 4` (expected `child: handler ran, exit 0`).
- **Proposed fix:** in `wasm_user_mode_tail`, when `flow & 1` comes with no user instance yet, remember it
  (`pending_signal = true`) and return; in `user_executable_setup`, once the instance exists (after
  `__wasm_apply_data_relocs` / `__set_tls_base`), run the same handler code (set `__stack_pointer` and the TLS base
  from the kernel, call `__libc_handle_signal`, catch the `signal_return` Trap, restore) before `_start` or
  `__libc_clone_callback`. Flow 3 cannot happen there (no handler can have returned yet).
- **Ours:** has it.

## 5. pthread_cancel() aborts the process, or never takes effect

- **Severity:** crash / hang.
- **Where:** musl `src/thread/pthread_cancel.c` as the upstream patch leaves it
  (`patches/musl/0001-NOMERGE-...patch`, around lines 2064-2152): cancellation points are plain system calls
  (`SYSCALL_CP_C` without the cancel checks), and `cancel_handler()` calls `abort()` when there is no `MC_PC`.
- **Cause:** `pthread_cancel()` sends SIGCANCEL; its handler on Wasm aborts the whole process whenever
  cancellation is enabled. If the target is blocked in a restartable call (`read()`), the handler does not even
  run (bug 3) and `pthread_join()` waits forever.
- **Repro:** `cancel.c` (a thread in `read()`, or with an argument in `sleep()`; main cancels and joins).
  Output: `/audit/cancel sleep` prints `cancelling`, `Aborted`, `rc=134`; `/audit/cancel` prints `cancelling` and
  hangs (expected `joined: PTHREAD_CANCELED (ok)` for both).
- **Proposed fix:** Wasm cannot move a program counter, so do the check in C: install SIGCANCEL without
  SA_RESTART on Wasm and make `cancel_handler()` only `return` (the flag `self->cancel` is already set); in
  `SYSCALL_CP_C`, check `self->cancel` before the call and `r == -EINTR && self->cancel` after it, and call
  `__cancel()` (when cancellation is enabled and the call is not `close`). The window between the first check and
  the system call remains (a cancel landing there waits for the next cancellation point); to close it the kernel
  would need a way to fail a call that starts with a cancel pending.
- **Ours:** has it.

## 6. fork() "succeeds" and the child dies with SIGILL

- **Severity:** crash (the child), wrong result (the parent sees a child that never ran its code).
- **Where:** musl `src/process/_Fork.c` (generic, `__syscall(SYS_clone, SIGCHLD, 0)`) against the Wasm clone
  protocol (`__libc_clone_callback` in `src/thread/wasm/clone.S`, which takes `fn` and `arg` from the new
  stack); the kernel's `copy_thread()` (`arch/wasm/kernel/process.c:190-231`) accepts a clone without `CLONE_VM`.
- **Cause:** a NOMMU kernel cannot give the child a copy of the memory, and the plain clone system call does not
  put a function on the stack, so the child's host calls `__libc_clone_callback`, which calls whatever is at the
  parent's stack pointer (`table index is out of bounds`).
- **Repro:** `fork.c`. Output: `fork returned 107, and the child was killed by signal 4 (WRONG)` (expected a
  failing `fork()`, so the program can fall back to `vfork()`/`posix_spawn()`, as on uClinux).
- **Proposed fix:** make `_Fork()` fail with `ENOSYS` in musl's Wasm port, and refuse a user clone without
  `CLONE_VM` in `copy_thread()`:
  ```c
  	} else {
  		/* User thread */
  		if (!(args->flags & CLONE_VM))
  			return -EINVAL;	/* NOMMU: no copy of the memory to give a child */
  ```
- **Ours:** has it (rv does its own fork by snapshot; native programs that call `fork()` crash like this).

## 7. a_cas_p() compares and stores only 32 bits of a 64-bit pointer (wasm64)

- **Severity:** corruption / hang.
- **Where:** musl `arch/wasm/atomic_arch.h:8-12`.
- **Cause:** `a_cas_p` casts the pointer slot to `volatile int *` and both pointers to `int`. On wasm64 it swaps
  the low half only (the high half keeps its old value) and returns the old low half sign-extended. Users in musl:
  `dcngettext.c` (`bindings`, and the `cats` loop `while (a_cas_p(&cats, old_cats, p) != old_cats)`, which spins
  for good once a catalog sits at or above 2 GiB, because the sign-extended return never equals the old value),
  `dlerror.c` (`freebuf_queue`), and the vDSO paths of `clock_gettime.c`/`sched_getcpu.c`/`cache.c` (unused on
  Wasm). Upstream's 4 GiB memory makes the sign extension (2-4 GiB) the live case; with more memory (our item 15)
  the truncation is too.
- **Repro:** `cas_p.c` (musl's definition copied, values never dereferenced). Output:
  `cas(0 -> 0x100001000): returned 0, slot now 0x1000 (WRONG: truncated)`,
  `cas(0x80001000 -> 0x1000): returned 0xffffffff80001000 (WRONG: sign-extended, ...)`; the fixed version below:
  `slot now 0x100001000 (ok)`, `returned 0x80001000 (ok)`.
- **Proposed fix:**
  ```diff
  --- a/arch/wasm/atomic_arch.h
  +++ b/arch/wasm/atomic_arch.h
   #define a_cas_p a_cas_p
   static inline void *a_cas_p(volatile void *p, void *t, void *s)
   {
  -	return (void *)a_cas((volatile int *)p, (long)t, (long)s);
  +	__atomic_compare_exchange_n((void *volatile *)p, &t, s, (_Bool)0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  +	return t;
   }
  ```
- **Ours:** has it.

## 8. musl's ucontext_t does not have the kernel's layout

- **Severity:** wrong result; can become corruption. A handler that reads `uc_mcontext` or `uc_stack` gets
  garbage, a signal it adds to `uc_sigmask` is not blocked afterwards, and what it writes into its idea of
  `uc_sigmask` lands on the kernel's `uc_stack`, which `rt_sigreturn` passes to `restore_altstack()` (a bad value
  there makes the sigreturn fail with SIGSEGV).
- **Where:** musl `arch/wasm/bits/signal.h:32-37` against the kernel's `asm-generic/ucontext.h` (used by
  `arch/wasm/kernel/signal.c`).
- **Cause:** musl has `{ uc_link; uc_sigmask; uc_stack; uc_mcontext; }`; the kernel writes
  `{ uc_flags; uc_link; uc_stack; uc_mcontext; uc_sigmask; }`.
- **Repro:** `ucontext.c`. Output through musl's type:
  `uc_stack.ss_flags 3 (want 2), ss_size 67108868 (want 0); uc_mcontext sp 0x6`,
  `SIGUSR2 added to uc_sigmask is NOT blocked after the handler (want blocked)`; through the kernel's layout:
  `ss_flags 2, ss_size 0, sp 0xebfdf0`, `SIGUSR2 added through the kernel's layout is blocked (ok)`.
- **Proposed fix:**
  ```diff
  --- a/arch/wasm/bits/signal.h
  +++ b/arch/wasm/bits/signal.h
   typedef struct __ucontext {
  +	unsigned long uc_flags;
   	struct __ucontext *uc_link;
  -	sigset_t uc_sigmask;
   	stack_t uc_stack;
   	mcontext_t uc_mcontext;
  +	sigset_t uc_sigmask;
   } ucontext_t;
  ```
  (`mcontext_t` already matches `struct sigcontext` = `user_regs_struct { stack_pointer, tls }`.)
- **Ours:** has it.

## 9. copy_thread() dereferences a kernel thread's NULL mm, and zeroes the parent's fields

- **Severity:** latent corruption.
- **Where:** `arch/wasm/kernel/process.c:221-228`.
- **Cause:** `if (!p->mm->binfmt) { current->mm->start_code = 0; ... }`. For every kernel thread `p->mm` is NULL,
  so this reads `binfmt` from the first 64 KiB of memory (empty space below `CONFIG_KERNEL_START`, which every
  stray NULL write of any program on the machine also lands in: Wasm has no null page fault). When that word is 0
  it then writes zeroes through `current->mm`, NULL again for kthreadd. The intended target was surely `p->mm`.
  Today it writes zeroes over zeroes; once something has written into the low 64 KiB (as #19 did) it either skips
  or writes at small addresses.
- **Repro:** none (by reading).
- **Proposed fix:**
  ```diff
  -	if (!p->mm->binfmt) {
  +	if (p->mm && !p->mm->binfmt) {
   		/* These are normally not zeroed out in copy_process(). */
  -		current->mm->start_code = 0;
  ...                                   (the same for end_code, start_stack, start_data, end_data: p->mm)
  ```
- **Ours:** has it.

## 10. futex_atomic_cmpxchg_inatomic() fails instead of reporting the value it found

- **Severity:** wrong result; a user program can make `futex_unlock_pi()` retry in the kernel without end.
- **Where:** `arch/wasm/include/asm/futex.h` (`futex_atomic_cmpxchg_inatomic`).
- **Cause:** the contract is: store the value found at `uaddr` in `*uval` and return 0, whether or not it equalled
  `oldval` (callers compare). This one tries the strong CAS 128 times and returns `-EAGAIN` on a mismatch without
  setting `*uval`. `wake_futex_pi()` turns "found a different value" into `-EAGAIN` (retry) or `-EINVAL` (garbage
  in the futex word) by looking at `curval`; it now always gets `-EAGAIN`, so a corrupt PI futex word makes
  `futex_unlock_pi()` retry forever instead of failing with EINVAL. Other callers (robust list cleanup,
  `lock_pi_update_atomic`) re-read and retry and are only slowed down.
- **Repro:** none (by reading).
- **Proposed fix:**
  ```c
  	expected = oldval;
  	__atomic_compare_exchange_n((volatile u32 *)uaddr, &expected, newval, false,
  				    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
  	*uval = expected;
  	return 0;
  ```
- **Ours:** has it.

## 11. Every trap becomes SIGILL

- **Severity:** wrong result (wrong exit status and messages: an out-of-bounds access says "Illegal instruction";
  a handler for SIGSEGV or SIGFPE is never chosen).
- **Where:** `arch/wasm/kernel/traps.c:174-189` and `wasm_error` (`linux-worker.js:310-322`), which does not pass
  what kind of error it was.
- **Proposed fix:** give `raise_exception()` an argument the host derives from the error: RuntimeError
  "memory access out of bounds" and RangeError "Maximum call stack size exceeded" give SIGSEGV, "divide by zero"
  / "integer overflow" / "invalid conversion to integer" give SIGFPE, "unreachable" / "null function or function
  signature mismatch" / "table index is out of bounds" give SIGILL.
- **Ours:** has it.

## 12. binfmt_wasm's auxiliary vector: AT_EGID is the real gid, AT_RANDOM is missing

- **Severity:** wrong result; weak stack-protector canary.
- **Where:** `fs/binfmt_wasm.c:49-58`.
- **Cause:** `AT_EGID, from_kgid_munged(cred->user_ns, cred->gid)` (should be `cred->egid`). There is no
  `AT_RANDOM`: musl's `__init_ssp()` then derives the canary from an address, and programs that read
  `getauxval(AT_RANDOM)` get 0.
- **Repro:** `auxv.c` (sets the effective gid to 5 and executes itself). Output:
  `getgid 0 getegid 5; AT_GID 0 AT_EGID 0 (WRONG); AT_RANDOM 0 (missing)`.
- **Proposed fix:** `cred->egid`; for AT_RANDOM, reserve 16 more bytes in `extra_stack`, fill them with
  `get_random_bytes()` above the argument strings and put their address in the vector.
- **Ours:** has it.

## 13. sys_mmap's offset is in 4 KiB units on wasm64, and only right while PAGE_SIZE is 4 KiB

- **Severity:** wrong result (latent: with the 64 KiB page size the Kconfig offers, or for any caller that passes
  bytes as the comment says).
- **Where:** `arch/wasm/kernel/sys_wasm.c:5-12`.
- **Cause:** the comment says the 64-bit `mmap` takes the offset in bytes, but the argument is named `pgoff` and
  passed straight to `ksys_mmap_pgoff()`. musl uses `SYS_mmap2` (number 222, which on the 64-bit kernel is this
  `sys_mmap`) and passes `off / 4096`, so the two agree only when PAGE_SIZE is 4096. Other 64-bit ports take bytes.
- **Repro:** `mmapoff.c` passes today (`mmap at offset 4096 begins with 'B' (ok)`), which shows the 4 KiB-unit
  agreement; the bug needs `CONFIG_PAGE_SIZE_64KB` or a byte-offset caller.
- **Proposed fix:** shift like `mmap2` does (`pgoff >> (PAGE_SHIFT - 12)`) and fix the comment, or switch musl to
  byte offsets and the kernel to `off >> PAGE_SHIFT` with an alignment check.
- **Ours:** has it (harmless at 4 KiB pages).

## 14. A program whose main() takes no arguments cannot start

- **Severity:** crash at start (valid C refused).
- **Where:** musl `arch/wasm/crt_arch.h:13-18`.
- **Cause:** crt1 defines `main(argc, argv, envp)` that calls `__main_argc_argv`, the name clang gives a
  `main(int, char **)`. For `int main(void)` clang emits `__original_main` and its own `main` shim instead, so
  `__main_argc_argv` stays an unresolved import (the link only warns: "function signature mismatch: main").
- **Repro:** `mainvoid.c`. Output: `LinkError: WebAssembly.instantiate(): Import #6 module="env"
  function="__main_argc_argv": function import requires a callable`, `Illegal instruction`, `rc=132`.
- **Proposed fix:** do what wasi-libc does: declare `__main_argc_argv` and `__main_void` (or `__original_main`)
  weak in crt1 and call whichever is defined; do not define `main` in crt1.
- **Ours:** has it.

## 15. Smaller things found while reading

- `fs/binfmt_wasm.c:297-306`: the dylink.0 `memoryalignment` is parsed and then ignored: the data area is only
  page aligned. A module asking for more (an `alignas(65536)` object, a large TLS alignment) is misaligned.
  `table_size` is `PAGE_ALIGN`ed (a count of table slots, not bytes) and never used.
- `fs/binfmt_wasm.c:111-138`: `wasm_consume_varU32()` returns `!(chunk & 0x80)` with `chunk` uninitialized when
  `count` is 0, and shifts an `int` by 35 on a sixth byte.
- `runtime/linux.js:201`: `last_task` is a `Uint32Array`, so a task_struct above 4 GiB is truncated on a switch
  through the main thread. Ours still has it on the `?switches=main` path (`linux.js:579`); the direct switch uses
  64-bit words.
- `include/unistd.h` (upstream musl patch): `syscall()` with 7 or more arguments returns the raw `-ENOSYS` (not -1
  with errno set).
- setjmp/longjmp, sigsetjmp/siglongjmp and vfork() abort (documented TODOs in the port); worth a line in the
  README, since `setjmp` silently "succeeds".

---

## Fixed in our tree, on neither list (upstream still has them)

From `patches/musl-wasm64.diff`, `patches/kernel-rvwasm.diff` and our commits; by reading, no new repro.

1. **musl `__clone()` drops `tls` and `ctid`** (`src/linux/clone.c`, upstream patch): they are read from the
   varargs only with `CLONE_CHILD_SETTID`, which `pthread_create()` does not pass. Every thread gets TLS 0 (its
   `_Thread_local` data, `__musl_tp` and errno included, at the bottom of memory, shared by all threads), and
   `CLONE_CHILD_CLEARTID` gets a NULL address, so `__thread_list_lock` (musl's ctid) is never released when a
   thread exits: the next `pthread_create()`/`pthread_exit()` deadlocks. Ours reads all three when any of the
   flags is set.
2. **A thread's Wasm TLS block** (`src/env/__init_tls.c`, `pthread_create.c`): `__tls_base` must point to a fresh
   copy of the TLS image (`__wasm_init_tls`), not to `struct pthread` (`TP_ADJ(new)`). Ours puts the block in
   front of the pthread struct and passes it as the clone's tls.
3. **`__unmapself()` aborts** (`src/thread/__unmapself.c` via `CRTJMP` = `abort()` in `arch/wasm/reloc.h`): every
   detached thread that exits kills its process with SIGABRT. Ours makes the two system calls directly.
4. **System calls with a different number of arguments than the handler trap the kernel**
   (`arch/wasm/kernel/traps.c:43-50`): the handler is called through `sys_call_table` cast to the caller's arity;
   a Wasm `call_indirect` with the wrong signature traps, in kernel mode. Ours: `ARCH_HAS_SYSCALL_WRAPPER`-style
   wrappers taking `pt_regs` (`arch/wasm/include/asm/syscall_wrapper.h`).
5. **Several pending signals stack frames of which only the top one's handler runs** (`signal.c`,
   commit 2a3e2ef): `exit_to_user_mode_loop()` set up a frame per pending signal; the host ran one handler, and its
   sigreturn left the program on the first frame with that handler never run and its mask never given back (the
   signal stays blocked for good). Ours sets up one frame per return.
6. **The main stack is two pages (8 KiB at 4 KiB pages), and brk grows into it** (`fs/binfmt_wasm.c:23, 359-373`,
   commit 45494db): one PATH_MAX buffer overruns it, and `brk()` hands out the same pages the stack grows down
   into. Ours: 64 pages, and `end_brk = start_brk` (malloc falls back to mmap).
7. **Two forks on two CPUs can be given the same CPU** (`user_task_set_affinity()`, `process.c:110-130`):
   `cpumask_first_zero()` then `cpumask_set_cpu()` without a lock (its own comment says so); two tasks pinned to one
   CPU, one starving while the other computes. Ours: `user_cpus_mutex`.
8. **`regs->syscall_args` keeps a previous call's arguments** past the ones passed (`traps.c:35` copies only
   `x` of them), which seccomp filters and audit see. Ours zeroes them first.

---

## Checked and found right

`__libc_handle_signal`'s SA_SIGINFO test on both widths (only the extra `iLONG.const 4` of #20 is wrong); the
kernel's signal frame alignment (frame size and `STACK_ALIGN` 16) and `sigsp()` for the alternate stack;
`rt_sigreturn` restoring the mask, stack pointer, TLS and alternate stack; `struct stat`/`kstat` on wasm64 (musl's
8-byte `nlink_t`/`blksize_t` match the kernel: `stat.c` prints the right mode, nlink 2, size 12); `SYS__llseek`
kept to wasm32; the IRQ word wait/notify in `arch_cpu_idle()` (no lost wake: the wait expects 0 and every raise
sets a bit before notifying); the host wrapper's `Number()`/`BigInt()` conversions (exact below 2^53, not 32-bit
truncation); an expired timer is delivered at the next system call return on our kernel (`alarmspin.c`: `handler
ran (ok)`; not checked on upstream's kernel, which has no IRQ poll at system call entry).
