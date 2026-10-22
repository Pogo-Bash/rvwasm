# Third-party components of the WebGPU executor (rvgpu)

For the public release's `THIRD-PARTY.md` (VK-05). `rvgpu.wasm` (`dist/www/rvgpu/`, `?exec=webgpu`) bundles the
components below, built by `gpu/webgpu/build/wasm.sh` from the pins in `gpu/webgpu/build/*.pin` and
`gpu/webgpu/vkr/virglrenderer.pin`; the stamp section of the binary (`node tools/rvgpu-stamp.js rvgpu.wasm`) says which
commit, Mesa pin and patch it was built from, and `RELEASE.txt` records it. Rows in the format of that file:

| Component | Where it is | Licence | Source |
|---|---|---|---|
| **Mesa** (Zink, the GL front end, NIR, util; wgvk and vfront are our own code in Mesa's layout) | compiled to WebAssembly into the page's GPU executor (`rvgpu`) | MIT (mostly; see Mesa's own licence notes) | <https://gitlab.freedesktop.org/mesa/mesa>, the commit in `gpu/webgpu/build/mesa.pin`, plus `patches/mesa-page.diff` and `gpu/webgpu/mesa/src/webgpu/` |
| **virglrenderer** (venus / vkr decoder, vrend) | compiled to WebAssembly into `rvgpu` | MIT | <https://gitlab.freedesktop.org/virgl/virglrenderer>, the commit in `gpu/webgpu/vkr/virglrenderer.pin`, plus `gpu/webgpu/vkr/virglrenderer-page.diff` |
| **venus-protocol** (generated command decoders) | compiled to WebAssembly into `rvgpu` | MIT | <https://gitlab.freedesktop.org/virgl/venus-protocol>, the tag in `gpu/webgpu/vkr/virglrenderer.pin` |
| **Dawn** (`webgpu.h`, emdawnwebgpu's JavaScript and C glue) | linked into `rvgpu.wasm` and `rvgpu.js`; its Node bindings (`dawn.node`) are used by the tests only, not shipped | BSD-3-Clause | <https://dawn.googlesource.com/dawn>, the commit in `gpu/webgpu/build/dawn.pin` |
| **Vulkan headers and registry** | build input (`vk.xml`, headers) | Apache-2.0 / MIT | <https://github.com/KhronosGroup/Vulkan-Headers>, as Mesa's tree carries them |
