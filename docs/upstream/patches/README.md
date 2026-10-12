# Patches for linux-wasm (from branch `lwfix`), against pristine upstream

For later issues or pull requests (nothing has been sent anywhere). Each directory is a series made with
`git format-patch`, to apply in order with `git am` on the pristine tree it names. The numbers in brackets are the
bugs of `docs/upstream/linux-wasm-audit.md`.

| Series | Applies to | Patch | Fixes |
|---|---|---|---|
| `kernel/` | joelseverin/linux `039e5f3e5` ("wasm: hvc: Add Wasm console support") | 0001 run the handler of a trap, with the signal the trap means | [1], [11] |
| | | 0002 end a task that runs its program when a fatal signal comes | [2] |
| | | 0003 restart an interrupted system call after its signal handler | [3] |
| | | 0004 refuse a clone that is a fork | [6] |
| | | 0005 copy_thread: no mm for a kernel thread | [9] |
| | | 0006 futex_atomic_cmpxchg_inatomic reports the value it found | [10] |
| | | 0007 binfmt_wasm: AT_EGID, AT_RANDOM | [12] |
| | | 0008 sys_mmap's offset in 4 KiB units whatever the page size | [13] |
| `runtime/` | joelseverin/linux-wasm `3103d5c` ("runtime: Unify wasm64 hack...") | 0001 a trap of the program runs its handler | [1], [11] (needs kernel 0001) |
| | | 0002 end the Worker of a task that runs its program | [2] (needs kernel 0002) |
| | | 0003 a signal at a new task's first return | [4] |
| `musl/` | linux-wasm's musl `5fca156` (the NOMERGE patch on v1.2.5) | 0001 a_cas_p swaps the whole pointer | [7] |
| | | 0002 ucontext_t in the kernel's layout | [8] |
| | | 0003 pthread_cancel without a program counter | [5] (with kernel 0003 for the restart order) |
| | | 0004 fork() fails with ENOSYS | [6] |
| | | 0005 programs whose main() takes no arguments start | [14] |

Kernel 0002 adds an import, `wasm_end_user_task(task, state)`: a runtime without runtime 0002 cannot start such a
kernel. (Our own tree, `patches/kernel-lwfix.diff` with `tools/patch-runtime.py` 21, signals the host through
`wasm_release_task()` with the task's address + 1 instead, so that older runtimes still start it; upstream, where
kernel and runtime come together, a proper import is cleaner.) Otherwise the kernel series is our
`patches/kernel-lwfix.diff` taken to pristine code (no `check_current()`, `wasm_flags_note()`, pool or
`syscall_wrapper.h` there); the musl series is the same code as ours (none of those files is touched by
`patches/musl-wasm64.diff`).

## How they were tested

- In our tree (`patches/kernel-rvwasm.diff` + `kernel-memsize.diff` + `kernel-lwfix.diff`, runtime 1-22, musl with
  `musl-wasm64.diff`): `tests/linux-wasm-audit/check.sh` under `tools/node-machine.js`, all 18 cases pass; with main's
  kernel, runtime and musl 14 of them fail.
- On pristine upstream with these series (kernel built with `wasm64_nommu_defconfig`, the runtime as patched, the
  reproducers linked with the patched musl, upstream's BusyBox initramfs), same check: fault, fault-blocked,
  spin-kill, spin-term, spinexit, spinexec, restart-eintr, earlysig, fork, cas_p, auxv, mainvoid pass. The others
  are held up by upstream bugs these series do not touch: trapsig, ucontext and the cancel cases use SA_SIGINFO
  handlers, which upstream musl calls wrongly on wasm64 (#20 of `docs/upstream/linux-wasm-issues.md`: they crash
  with SIGSEGV); mmapoff fails with EBADF (the system call argument mismatch, #21); and the SA_RESTART case of
  restart hangs on pristine upstream with or without kernel 0003 (its writer thread's write never arrives there),
  so 0003 is shown working in our tree only.
- The kernel series compiles without new warnings (`-Wmissing-prototypes` included) with upstream's toolchain.
