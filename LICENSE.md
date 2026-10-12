> **Not legal advice; may be revised.**

# rvwasm's licence layout

**rvwasm's own code is licensed under the GNU General Public License, version 2 or (at your option) any later
version (`GPL-2.0-or-later`); the full text of version 2 is in `COPYING`. Third-party components, and files
derived from them, keep their own licences.**

(rvwasm was first published under a noncommercial licence. That was changed to the GPL on 2026-10-12: the page
runs our code together with linux-wasm's GPL-2.0-only runtime as one program, and the GPL allows no extra
restrictions on such a program.)

Copyright (c) 2026 Swap (https://plasmaontheweb.com)

## Which paths are under which licence

Where a file carries an `SPDX-License-Identifier` line, that line governs the file. Otherwise this table does.
"GPL-2.0-or-later" below means version 2 of the GPL (`COPYING`) or any later version.

| Path | Licence | Why |
|---|---|---|
| `rv/`, `wlserver/*.js`, `wlserver/*.md`, `gpu/` (except what is listed below), `hostnode/`, `hostopen/`, `probes/`, `tests/`, `docker/`, `docker-compose.yml`, `site/`, `docs/*.md` and `docs/usability-audit-2026-10-10/`, `results/`, and `tools/` (except what is listed below) | GPL-2.0-or-later | rvwasm's own code and writing |
| `gpu/vdec/` (the video decoder driver) | MIT (its file headers) | own code |
| `blenderweb/` (except `blenderweb/www/`) | GPL-2.0-or-later | own code; it fetches and runs Puter's Blender build, which is separate (below) |
| `blenderweb/www/main.js` | GPL-2.0 (its header; Puter's README says 2 or later) | Puter's blender-wasm demo, changed |
| `blenderweb/www/index.html` | GPL-2.0-or-later (its own header says so) | the page that starts Puter's Blender |
| `patches/kernel-*.diff`, `docs/upstream/patches/kernel/` | GPL-2.0-only | patches to the Linux kernel as built by linux-wasm; the new files carry `GPL-2.0-only` headers |
| `patches/linux-wasm-runtime.diff`, `patches/linux-wasm-script.diff`, `docs/upstream/patches/runtime/`, `tools/patch-runtime.py`, `tools/www/dev.html` | GPL-2.0-only | derived from Joel Severin's linux-wasm (`runtime/` and `linux-wasm.sh`), which is `GPL-2.0-only` |
| `patches/musl-wasm64.diff`, `docs/upstream/patches/musl/` | MIT | patches to musl libc |
| `gpu/vrend/virglrenderer-wasm.diff` | MIT | patch to virglrenderer |
| `rv/virtgpu_formats.h` | MIT (generated from Mesa's `virgl_hw.h`) | derived from Mesa 25.0.7 |
| `wlserver/protocol/*.xml` | each file's own header: MIT, MIT-CMU or LGPL-2.1-or-later (Wayland, wlroots, KDE) | copies of the protocol descriptions |
| `wlserver/keymaps/*.xkb` | the xkeyboard-config licence (MIT style) | compiled from xkb-data |
| `tools/x/nq-share/` | LGPL-2.1-or-later and the licences in each file (KDE Milou, KDE's default panel layout) | modified KDE files |
| `third_party/berkeley-softfloat-3` | BSD-3-Clause | Berkeley SoftFloat 3, a git submodule |
| `native/` (build scripts, `crt/`, `toolchain.cmake`, `nq-plugins.cmake`, `static-plugins.py`, `tests/`, `qml-stubs/`) and `docs/native-wasm.md` | GPL-2.0-or-later | rvwasm's own tooling for building Qt, KDE Frameworks and Plasma as native WebAssembly programs |
| `native/crt/nqsjlj.c` | MIT or UoI-NCSA (the Emscripten licence) | follows Emscripten's `emscripten_setjmp.c` (provenance not certain; see `THIRD-PARTY.md`) |
| `native/plasma-workspace-extra.cmake`, `native/plasma-workspace-end.cmake`, `native/plasma-desktop-parts/`, `native/plasma-shell-only.py` | GPL-2.0-or-later | pieces of KDE Plasma's build files and sources, modified (they are inserted into plasma-workspace and plasma-desktop) |
| `native/patches/nativeqt-qtbase.diff`, `nativeqt-qtdeclarative.diff` | LGPL-3.0-only, GPL-2.0-only or GPL-3.0-only, as Qt (the patches are offered under the licence of the file they change) | patches to Qt 6.8.2 |
| `native/patches/nativeqt-k*.diff`, `nativeqt-solid.diff`, `nativeqt-libplasma.diff`, `nativeqt-libksysguard.diff`, `nativeqt-plasma5support.diff`, `nativeqt-ksvg.diff`, `nativeqt-kirigami-addons.diff`, `nativeqt-extra-cmake-modules.diff` | the licence of the file each patch changes (LGPL-2.0-or-later, LGPL-2.1-or-later, GPL-2.0-or-later, BSD-3-Clause for ECM) | patches to KDE Frameworks 6.13, Plasma 6.3.6 and their add-ons |
| `native/patches/nativeqt-layer-shell-qt.diff` | LGPL-2.1-only or LGPL-3.0-only (as layer-shell-qt) | patch to layer-shell-qt |
| `native/patches/nativeqt-mesa.diff`, `nativeqt-wayland.diff`, `nativeqt-libxkbcommon.diff` | MIT | patches to Mesa, libwayland, libxkbcommon |
| `native/patches/nativeqt-llvm18-runtimes.diff` | Apache-2.0 with LLVM exception | patch to libc++ and libc++abi 18 |
| `native/patches/nativeqt-sqlite.diff` | public domain (as SQLite) | patch to SQLite 3.46.1 |

`THIRD-PARTY.md` lists the components that are in the built release and where their source is. `native/README.md` says how to rebuild the native desktop shell from the published sources.

Patches that rvwasm offers back to an upstream project (`docs/upstream/patches/`) are offered under that
project's licence, as the table says.

Names are not licensed here: see `TRADEMARKS.md`.

