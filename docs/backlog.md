# Backlog

Ideas the owner wants kept for later. Not started; each needs the owner's go.

## RISC-V vector extension (RVV) in rv, on wasm SIMD128

Added 2026-10-11.

rv emulates RV64GC, without the V extension. Debian riscv64 is built for RV64GC, so everything runs,
but libraries that pick vector code at run time when the CPU reports RVV take their scalar paths
under rv: FFmpeg/dav1d, OpenSSL, image libraries, some glibc string and memory routines.

The idea: implement RVV 1.0 in rv with VLEN=128, translated to WebAssembly SIMD128 (which V8 compiles
to the host's SIMD instructions). With VLEN=128 most vector operations map one to one onto v128
operations; LMUL > 1 becomes several v128 values, and masks, tail/mask agnosticism, vsetvl and
segment loads/stores are the hard parts.

Work involved:
- decoding and an interpreter path for the V instructions; vector register state and vsetvl;
- JIT translation to v128 operations, plus the cases that need scalar loops;
- reporting V to the guest (hwcap, /proc/cpuinfo isa string, riscv_hwprobe) only once it is complete
  enough, since libraries trust it;
- tests against qemu-riscv64 with V enabled.

Background: Linux has supported RVV for user space since 6.5; compiling the RISC-V kernel itself with
auto-vectorization was a Linux Plumbers Conference 2026 talk (https://lpc.events/event/20/contributions/2455/).
That kernel part does not apply here: rvwasm's kernel is linux-wasm, not RISC-V.

Priority: low for now. Video decoding moves to the browser's WebCodecs (branch vdec), which takes the
biggest vector workload off the CPU path.
