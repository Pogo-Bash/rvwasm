# The `vulkan` branch (VK-05)

`vulkan` = `release` + `webgpu`, the integration branch of the Vulkan/GPU program. It was cut from release 8d1798c with
webgpu 9e4bdaf merged in (merge commit 3192045). It is not merged into `release` until the plan's step 6.2 (flag-off
release); `rerere` is on in the worktree, so repeating a merge reuses the recorded resolutions.

**Rules for later cards**
- wgvk/rvgpu cards (`gpu/webgpu/`, `patches/mesa-page.diff`, `tools/www/rvgpu-worker.js`) work on `webgpu`. After each,
  merge `webgpu` into `vulkan` (`git merge webgpu`; a two-way merge, not a rebase). Page, gpuserver, desktop-script and
  release-script cards work on `vulkan` directly. Do not edit `gpu/webgpu/` on `vulkan`: it stays byte-identical to
  `webgpu` (`git diff webgpu vulkan -- gpu/webgpu` is empty) so the merges stay trivial.
- Release already carried webgpu's shared-file work (gpu/server/server.js, tools/www/gpu-exec.js, display.js, stage.sh via
  `gpuplus`). If a merge conflicts there, release's side is the newer one (the first merge: server.js RV_FENCE /
  RV_RESOURCE_IMPORT / RES_ATTACH / vdec hooks, gpu-exec.js "hidden" message).
- The default executor stays vrend: `tools/www/gpu-exec.js` starts `execParam || "vrend"`.

**Release scripts take rvgpu only from a stamped, clean build.** `tools/pack-www.sh` and `docker/release.sh dist` run
`tools/rvgpu-gate.sh` (`tools/rvgpu-stamp.js --require-clean`; `tools/rvgpu-stamp-check.py`, the same rule, where there is no
Node). An unstamped, dirty or commit-less `rvgpu.wasm` stops the release; `RV_RVGPU=skip` releases without the WebGPU
executor. `dist/RELEASE.txt` gets a line `rvgpu (WebGPU executor): BRANCH@COMMIT mesa M dawn D patch P, sha256 …`.
`tests/rvgpu-gate.sh` checks the gate. Third-party rows for the public `THIRD-PARTY.md`: `docs/third-party-rvgpu.md`.

**meson caches the cross file:** after a change to `gpu/webgpu/build/*.sh`'s cross file, `rm -rf b/NAME` once in the VM.
**vm.sh keeps mtimes:** after a merge, touch the sources that changed, or ninja tests stale code.
