# Paths for the SHARED (dylink.0 side modules) native Qt build. Sourced in the build VM. Everything lives
# beside the static build, never in it: sysroot-shared, build-shared. The static build's source trees are
# read, not written (qt-prep.sh already patched them).
LW=$HOME/rvwasm/linux-wasm/workspace
NQ_LLVM=$LW/install/llvm/bin
NQ=$HOME/rvwasm/nativeqt
NQ_SRC=$NQ/src                       # shared with the static build, read-only here
NQ_BUILD=$NQ/build-shared
NQ_SYSROOT=$NQ/sysroot-shared
NQS_TREE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)     # .../nativeqt/shared
NQ_TREE=$(cd "$NQS_TREE/.." && pwd)                        # .../nativeqt (nq-clang lives here)
NQ_JOBS=${NQ_JOBS:-6}
export NQ NQ_SYSROOT NQ_LLVM NQS_TREE NQ_TREE
export PATH=$NQS_TREE/bin:$NQ_TREE/bin:$PATH
