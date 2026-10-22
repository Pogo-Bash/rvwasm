# Dynamic libraries prototype

The design and the measurements are in `docs/wasm-dynamic-linking.md`. This directory is the
prototype: a dynamic linker for native linux-wasm programs (`ld-wasm`), the runtime's part of it, a
test library and test programs.

## Build (in the build VM)

Needs the nativeqt sysroot and toolchain (`~/rvwasm/nativeqt/sysroot`, `~/wt/nativeqt` for
`nq-clang` and `wasm-imports.py`; branch nativeqt). Copy this directory to the VM and build there:

    rsync -a -e "ssh -F ~/.lima/rvwasm/ssh.config" tests/wasmdyn lima-rvwasm:wt/wasmdyn/tests/
    limactl shell --workdir / rvwasm bash -lc 'cd ~/wt/wasmdyn/tests/wasmdyn && ./build.sh'

Output in `out/` there: `ld-wasm`, `libfoo.so`, `libbig*.so`, `main1`..`main5`, `bench`,
`bench-static`, `hello`, `hello-static`, `timeit`. `DYNSYM= ./build.sh` keeps the modules' wasm
exports (no `--dynsym`), for comparison.

## Run (on the Mac, under node-machine)

    tests/wasmdyn/run.sh 'cd /wasmdyn && ./ld-wasm ./main1' [TIMEOUT_MS]

`run.sh` fetches `out/` from the VM, puts it at `/wasmdyn` in an extra initramfs, takes the dev
server's kernel and initramfs, builds the runtime (linux-wasm's own, patched by `tools/patch-runtime.py`;
dynamic libraries are item 26, on with `?dl=1`, which run.sh passes; `DL=0` leaves them off), waits until no
other node-machine runs on the Mac, and runs the command (5-minute hard limit). `HOOK=tests/wasmdyn/dl-hook.js`
prints the module cache's statistics after the command; `PRELOAD=file.json` compiles libraries first. `LD_DEBUG=1` makes ld-wasm
say what it loads, how long each step took, and what each new thread loaded.

| command | what it checks |
|---|---|
| `./ld-wasm ./main1` | a C++ program linked against libfoo.so: data, calls, pointers both ways, virtual calls, dynamic_cast, exceptions both ways, TLS, constructors (22 checks) |
| `./ld-wasm ./main2 /wasmdyn/libfoo.so` | dlopen/dlsym/dlclose/dladdr/dlerror, RTLD_LOCAL then RTLD_GLOBAL (14) |
| `./ld-wasm ./main3 /wasmdyn/libfoo.so` | a thread started before dlopen calls into the library; per-thread TLS; a thread started after (8) |
| `./ld-wasm ./main4 /wasmdyn/libfoo.so` | a vfork-style child calls into a dlopen()ed library on its parent's TLS; posix_spawn (6) |
| `./ld-wasm ./main5 /wasmdyn/libbig.so` | dlopen and thread-start cost of a library with 20000 exported functions (also `libbig-js.so`, `libbig-noexp.so`, and `libbigcall.so bigcall_all`: 20000 imports) |
| `./ld-wasm ./bench` and `./bench-static` | call costs across modules against inside one |
| `./timeit 20 ./hello-static`, `./timeit 20 ./ld-wasm ./hello` | start-up |

Results of 2026-10-11 are in the design document, section 6 and 9.

## Files

- `ldso/ldso.c`: the dynamic linker (and the C library's `dlopen`/`__dlsym`/`dladdr`), linked with
  musl, libc++ and libc++abi into `ld-wasm`; `ldso/ldso_asm.S`: its `__memory_base`;
  `ldso/wasm_dl.h`: the descriptor the host reads; `ldso/dlcrt.S`: per-module TLS helpers, linked
  into every module.
- `check-cache.sh`: the per-page module cache (11 checks: libfoo compiled once across programs, threads and
  children; preload; dl off); `dl-hook.js`; `../../tools/dl-hash.py` (content keys).
- `tablify.py`: post-link step: every exported function in a table slot by the module's own element
  segment; with `--dynsym` the exports move to a custom section.
- `env.sh`: compiler flags (as nativeqt's `nq-clang`).
