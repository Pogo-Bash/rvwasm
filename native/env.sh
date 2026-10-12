# Paths for the native (wasm64 linux-wasm) Qt/KDE builds. Sourced by the build scripts, in the build VM.
LW=$HOME/rvwasm/linux-wasm/workspace
NQ_LLVM=$LW/install/llvm/bin
NQ=$HOME/rvwasm/nativeqt            # big things: sources, builds, the sysroot
NQ_SRC=$NQ/src
NQ_BUILD=$NQ/build
NQ_SYSROOT=$NQ/sysroot              # musl + kernel headers + libc++ + everything we build, prefix /usr
NQ_TREE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NQ_JOBS=${NQ_JOBS:-10}
export NQ NQ_SYSROOT NQ_LLVM
export PATH=$NQ_TREE/bin:$PATH
