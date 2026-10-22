# Sourced by the build scripts (in the build VM). Sets:
#   WT     this worktree (the repo checkout the script runs from)
#   NAME   the build's name: $1 of env.sh, else $GPU_BUILD, else the worktree's directory name
#   W      ~/rvwasm/webgpu (shared VM assets: Dawn, the pinned Mesa clone, emdawnwebgpu, the venv)
#   PIN    the pinned Mesa commit (gpu/webgpu/build/mesa.pin)
#   TREE   this build's Mesa tree (a git worktree of $W/mesa-main at PIN, patches/mesa-page.diff applied,
#          src/webgpu -> $WT/gpu/webgpu/mesa/src/webgpu)
#   B      this build's output directory: $B/native, $B/wasm, $B/rvgpu
WT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
NAME=${GPU_BUILD:-$(basename "$WT")}
W=$HOME/rvwasm/webgpu
PIN=$(grep -v '^#' "$WT/gpu/webgpu/build/mesa.pin" | head -1 | cut -d' ' -f1)
TREE=$W/trees/$NAME
B=$W/b/$NAME
JOBS=${JOBS:-6}
export PATH=$W/venv/bin:$PATH
export PKG_CONFIG_PATH=$W/pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
