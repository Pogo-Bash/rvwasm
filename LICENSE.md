> **Draft: not legal advice; to be reviewed by a lawyer before publication.**
> This top note is not part of the licence text below. The licence text starts at the line
> "# rvwasm Noncommercial Share-Alike License 1.0".

# rvwasm's licence layout

**rvwasm's own code is licensed under the rvwasm Noncommercial Share-Alike License 1.0, below. Third-party
components, and files derived from them, keep their own licences.** The licence below is for noncommercial use
only, and it asks that anything built on rvwasm and shared with others is published as source under the same
terms.

Required Notice: Copyright (c) 2026 Swap (https://plasmaontheweb.com)

## Which paths are under which licence

Where a file carries an `SPDX-License-Identifier` line, that line governs the file. Otherwise this table does.
`LicenseRef-rvwasm-NC-SA-1.0` means the licence below.

| Path | Licence | Why |
|---|---|---|
| `rv/`, `wlserver/*.js`, `wlserver/*.md`, `gpu/` (except what is listed below), `hostnode/`, `hostopen/`, `probes/`, `tests/`, `docker/`, `docker-compose.yml`, `site/`, `docs/*.md` and `docs/usability-audit-2026-10-10/`, `results/`, and `tools/` (except what is listed below) | the licence below | rvwasm's own code and writing |
| `gpu/vdec/` (the video decoder driver) | the licence below | own code (older file headers said MIT; changed before publication) |
| `blenderweb/` (except `blenderweb/www/`) | the licence below | own code; it fetches and runs Puter's Blender build, which is separate (below) |
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
| `native/` (build scripts, `crt/`, `toolchain.cmake`, `nq-plugins.cmake`, `static-plugins.py`, `tests/`, `qml-stubs/`) and `docs/native-wasm.md` | the licence below | rvwasm's own tooling for building Qt, KDE Frameworks and Plasma as native WebAssembly programs |
| `native/crt/nqsjlj.c` | MIT or UoI-NCSA (the Emscripten licence) | follows Emscripten's `emscripten_setjmp.c` (provenance not certain; see `THIRD-PARTY.md`) |
| `native/plasma-workspace-extra.cmake`, `native/plasma-workspace-end.cmake`, `native/plasma-desktop-parts/`, `native/plasma-shell-only.py` | GPL-2.0-or-later | pieces of KDE Plasma's build files and sources, modified (they are inserted into plasma-workspace and plasma-desktop) |
| `native/patches/nativeqt-qtbase.diff`, `nativeqt-qtdeclarative.diff` | LGPL-3.0-only, GPL-2.0-only or GPL-3.0-only, as Qt (the patches are offered under the licence of the file they change) | patches to Qt 6.8.2 |
| `native/patches/nativeqt-k*.diff`, `nativeqt-solid.diff`, `nativeqt-libplasma.diff`, `nativeqt-libksysguard.diff`, `nativeqt-plasma5support.diff`, `nativeqt-ksvg.diff`, `nativeqt-kirigami-addons.diff`, `nativeqt-extra-cmake-modules.diff` | the licence of the file each patch changes (LGPL-2.0-or-later, LGPL-2.1-or-later, GPL-2.0-or-later, BSD-3-Clause for ECM) | patches to KDE Frameworks 6.13, Plasma 6.3.6 and their add-ons |
| `native/patches/nativeqt-mesa.diff`, `nativeqt-wayland.diff`, `nativeqt-libxkbcommon.diff` | MIT | patches to Mesa, libwayland, libxkbcommon |
| `native/patches/nativeqt-llvm18-runtimes.diff` | Apache-2.0 with LLVM exception | patch to libc++ and libc++abi 18 |
| `native/patches/nativeqt-sqlite.diff` | public domain (as SQLite) | patch to SQLite 3.46.1 |

`THIRD-PARTY.md` lists the components that are in the built release and where their source is. `native/README.md` says how to rebuild the native desktop shell from the published sources.

Patches that rvwasm offers back to an upstream project (`docs/upstream/patches/`) are offered under that
project's licence, as the table says.

Names are not licensed here: see `TRADEMARKS.md`.

---

# rvwasm Noncommercial Share-Alike License 1.0

This licence is based on the PolyForm Noncommercial License 1.0.0
(<https://polyformproject.org/licenses/noncommercial/1.0.0>), which is the work of the PolyForm Project. It is
**modified** from that text (a Share-Alike section is added and the notice URL is changed), so it is not the
PolyForm Noncommercial License and it is not endorsed by, or affiliated with, the PolyForm Project.

## Acceptance

In order to get any license under these terms, you must agree to them as both strict obligations and conditions to all your licenses.

## Copyright License

The licensor grants you a copyright license for the software to do everything you might do with the software that would otherwise infringe the licensor's copyright in it for any permitted purpose.  However, you may only distribute the software according to [Distribution License](#distribution-license) and make changes or new works based on the software according to [Changes and New Works License](#changes-and-new-works-license).

## Distribution License

The licensor grants you an additional copyright license to distribute copies of the software.  Your license to distribute covers distributing the software with changes and new works permitted by [Changes and New Works License](#changes-and-new-works-license).

## Notices

You must ensure that anyone who gets a copy of any part of the software from you also gets a copy of these terms (this file, or the URL of its copy in the project's repository), as well as copies of any plain-text lines beginning with `Required Notice:` that the licensor provided with the software.  For example:

> Required Notice: Copyright (c) 2026 Swap (https://plasmaontheweb.com)

## Changes and New Works License

The licensor grants you an additional copyright license to make changes and new works based on the software for any permitted purpose.

## Share-Alike

You may not incorporate this software into any project that is not fully source-available and publicly available. Any modification, extension or derivative work that you distribute or make available to others, including over a network, must be published with its complete corresponding source code, publicly and at no charge, under this same license.

In plain words: if you build on rvwasm and let other people use what you built, you publish all of its source, for free, under this licence. The licence stays noncommercial, so the published source stays noncommercial too. It does not ask you to use any other licence for it.

## Patent License

The licensor grants you a patent license for the software that covers patent claims the licensor can license, or becomes able to license, that you would infringe by using the software.

## Noncommercial Purposes

Any noncommercial purpose is a permitted purpose.

## Personal Uses

Personal use for research, experiment, and testing for the benefit of public knowledge, personal study, private entertainment, hobby projects, amateur pursuits, or religious observance, without any anticipated commercial application, is use for a permitted purpose.

## Noncommercial Organizations

Use by any charitable organization, educational institution, public research organization, public safety or health organization, environmental protection organization, or government institution is use for a permitted purpose regardless of the source of funding or obligations resulting from the funding.

## Fair Use

You may have "fair use" rights for the software under the law. These terms do not limit them.

## No Other Rights

These terms do not allow you to sublicense or transfer any of your licenses to anyone else, or prevent the licensor from granting licenses to anyone else.  These terms do not imply any other licenses.

## Patent Defense

If you make any written claim that the software infringes or contributes to infringement of any patent, your patent license for the software granted under these terms ends immediately. If your company makes such a claim, your patent license ends immediately for work on behalf of your company.

## Violations

The first time you are notified in writing that you have violated any of these terms, or done anything with the software not covered by your licenses, your licenses can nonetheless continue if you come into full compliance with these terms, and take practical steps to correct past violations, within 32 days of receiving notice.  Otherwise, all your licenses end immediately.

## No Liability

***As far as the law allows, the software comes as is, without any warranty or condition, and the licensor will not be liable to you for any damages arising out of these terms or the use or nature of the software, under any kind of legal claim.***

## Definitions

The **licensor** is the individual or entity offering these terms, and the **software** is the software the licensor makes available under these terms.

**You** refers to the individual or entity agreeing to these terms.

**Your company** is any legal entity, sole proprietorship, or other kind of organization that you work for, plus all organizations that have control over, are under the control of, or are under common control with that organization.  **Control** means ownership of substantially all the assets of an entity, or the power to direct its management and policies by vote, contract, or otherwise.  Control can be direct or indirect.

**Your licenses** are all the licenses granted to you for the software under these terms.

**Use** means anything you do with the software requiring one of your licenses.
