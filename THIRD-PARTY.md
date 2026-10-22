# Third-party components

> **Not legal advice; may be revised.**

rvwasm's own code is under GPL-2.0-or-later (`LICENSE.md`, `COPYING`). The built release
(`dist/`: the page, the kernel, the boot image and the system disk) also contains other people's software, which
keeps its own licences. This file lists it, with the licence and where to get the **corresponding source**, as
the GPL and LGPL require for binaries that are distributed. Nothing in `LICENSE.md` limits your rights under any
licence named here.

How to get source in general:

- **Debian packages** (everything on the system disk that is not listed separately): the Debian source package of
  each binary package. `apt-get source PACKAGE` with `deb-src` lines for trixie, or
  <https://sources.debian.org/> and <https://snapshot.debian.org/> for the exact versions. The package list and
  versions of a disk are in its `/var/lib/dpkg/status`.
- **Our patches and build scripts:** this repository (`patches/`, `docs/upstream/patches/`, `tools/`, `docker/`).
  `docker/Dockerfile.build` and `docker/build.sh` rebuild the toolchain, the kernel, musl, BusyBox and the page.
- **Written offer:** for three years from the day you received a binary from us, we will send the complete
  corresponding source of any GPL or LGPL component in it, on request, for the cost of the medium. Ask by
  opening an issue at <https://github.com/Pogo-Bash/rvwasm>.

## Components

| Component | Where it is | Licence | Source |
|---|---|---|---|
| **linux-wasm** (Joel Severin): the kernel's WebAssembly port, its JavaScript runtime and build script | the kernel (`vmlinux`), `linux.js`, `linux-worker.js` | GPL-2.0-only (runtime and script); the kernel is GPL-2.0-only with the Linux syscall note | <https://github.com/joelseverin/linux-wasm>, at the commit `docker/Dockerfile.build` names; our changes: `patches/kernel-*.diff`, `patches/linux-wasm-runtime.diff`, `patches/linux-wasm-script.diff`, `tools/patch-runtime.py` |
| **Linux kernel** | inside the above | GPL-2.0-only | <https://kernel.org/>, the version linux-wasm's script fetches, plus the patches above |
| **musl libc** | the native WebAssembly programs (rv, the window server's helpers, BusyBox, the native desktop shell) | MIT | <https://musl.libc.org/>, as linux-wasm builds it, plus `patches/musl-wasm64.diff` |
| **BusyBox** | the boot image | GPL-2.0-only | <https://busybox.net/>, as linux-wasm builds it |
| **Berkeley SoftFloat 3** | linked into `rv` | BSD-3-Clause | the submodule `third_party/berkeley-softfloat-3` (<https://github.com/ucb-bar/berkeley-softfloat-3>) |
| **LLVM / clang / lld** | build tool; its runtime libraries may be linked into native WebAssembly programs | Apache-2.0 with LLVM exception | built by linux-wasm's script |
| **virglrenderer** (vrend) | compiled to WebAssembly in the page's GPU executor (`rvgpu`) | MIT | <https://gitlab.freedesktop.org/virgl/virglrenderer>, 1.1.1, plus `gpu/vrend/virglrenderer-wasm.diff` |
| **Emscripten** | build tool of the GPU executor | MIT / NCSA | <https://emscripten.org/> |
| **Mesa** (virgl driver, GBM, EGL, llvmpipe) | the system disk | MIT (mostly; see Mesa's own licence notes) | Debian's `mesa` source package; `rv/virtgpu_formats.h` is generated from Mesa's `virgl_hw.h` |
| **libva** and **vainfo** | the system disk; the video driver in `gpu/vdec/driver` is built against its headers | MIT | Debian's `libva` and `libva-utils` source packages |
| **mpv** | the system disk | GPL-2.0-or-later (Debian's build) | Debian's `mpv` source package |
| **PulseAudio** and **ALSA tools** | the system disk | LGPL-2.1-or-later (daemon and libraries; some modules GPL); ALSA tools GPL-2.0-or-later | Debian's `pulseaudio` and `alsa-utils` source packages |
| **Qt 6** | the native desktop shell is Qt statically built for WebAssembly; the system disk has Debian's Qt | LGPL-3.0-only, GPL-2.0-only or GPL-3.0-only (by module) | Debian's `qt6-*` source packages; the WebAssembly build: see "Native desktop shell" below |
| **KDE Frameworks 6** | same | mostly LGPL-2.0-or-later or LGPL-2.1-or-later; some GPL, MIT or BSD (per file: the `SPDX` lines) | Debian's `kf6-*` source packages; <https://invent.kde.org/frameworks> |
| **KDE Plasma** (plasmashell, KWin, System Settings, widgets) and **KDE applications** (Konsole, Dolphin, Kate) | same | GPL-2.0-or-later, GPL-3.0-or-later, LGPL-2.1-or-later (per file) | Debian's `plasma-*`, `kwin-*`, `konsole`, `dolphin`, `kate` source packages; <https://invent.kde.org/plasma> |
| **Wayland** and **KDE Wayland protocol** descriptions | `wlserver/protocol/*.xml` (copies, kept with their headers) | MIT, MIT-CMU, LGPL-2.1-or-later (per file) | <https://gitlab.freedesktop.org/wayland/>, <https://invent.kde.org/libraries/plasma-wayland-protocols>, wlroots |
| **xkeyboard-config** (xkb-data) | `wlserver/keymaps/*.xkb` are compiled from it | MIT style, copyright the X.Org and XKB contributors | Debian's `xkeyboard-config` source package |
| **KDE Milou and the default panel layout** | `tools/x/nq-share/` (modified copies) | LGPL-2.1-or-later and the per-file `SPDX` lines | <https://invent.kde.org/plasma/milou>, <https://invent.kde.org/plasma/plasma-desktop> |
| **Firefox ESR** | the system disk | MPL-2.0 (with others, per file) | Debian's `firefox-esr` source package |
| **Chromium** | the system disk, from the RevyOS repository (ISCAS), which rebuilds Debian trixie's package for riscv64 | BSD-3-Clause and many others | RevyOS's source packages at <https://mirror.iscas.ac.cn/revyos/> and <https://github.com/revyos>; Debian's `chromium` |
| **Node.js**, **Python**, **git**, **GCC** and the other packages in `DEV_PACKAGES` (`tools/build-debian-image.sh`) | the system disk | MIT, PSF-2.0, GPL-2.0, GPL-3.0-or-later (per package) | Debian source packages |
| **Node.js's JavaScript library** | run unchanged by `hostnode` (the window server and GPU server run on it in the machine); taken at build time from the Debian `nodejs` package by `hostnode/extract.js`, not stored here | MIT (Node.js; its bundled third-party parts have their own licences) | Debian's `nodejs` source package; <https://github.com/nodejs/node> |
| **xterm.js** | the development page's terminal | MIT | <https://github.com/xtermjs/xterm.js>, as linux-wasm ships it |

## Used by the server, the X desktop images and the tests

Not part of the built `dist/` that visitors download, but part of the project's deployment and checks:

| Component | Used for | Licence | Source |
|---|---|---|---|
| **Caddy** | HTTPS and static files (`docker-compose.yml`, its official image) | Apache-2.0 | <https://github.com/caddyserver/caddy> |
| **dnsmasq**, **nftables** | the relay's allowlisted DNS and firewall (`docker/Dockerfile.run`, Debian packages) | GPL-2.0-or-later / GPL-3.0 (dnsmasq), GPL-2.0-only (nftables) | Debian's `dnsmasq` and `nftables` source packages |
| **IceWM**, **twm**, **xterm**, **X.Org** (server, fbdev and evdev drivers) | the lighter X desktop images (`tools/build-debian-image.sh x-root`) | LGPL-2.0, MIT/X11 style | Debian source packages |
| **Catppuccin** palette | the landing page's colours (`site/`) | MIT | <https://github.com/catppuccin/catppuccin> |
| **FFmpeg** (libavcodec) | reference decoder and test-clip maker in `gpu/vdec/test/`; not in the release | LGPL-2.1-or-later / GPL-2.0-or-later | <https://ffmpeg.org/> |
| **QEMU** | the baseline the emulator is benchmarked against (`tools/build-qemu-*.sh`); not in the release | GPL-2.0-only | <https://www.qemu.org/> |

## Native desktop shell (Qt, KDE Frameworks, Plasma compiled to WebAssembly)

The release's `plasmashell` is Plasma 6.3.6 with Qt 6.8.2 and KDE Frameworks 6.13.0 built **statically** into one
WebAssembly program. What is needed to rebuild it, with the same sources and patches, is in this repository:

- `native/`: the toolchain (`nq-clang`), the per-package build scripts and their order (`native/run/overnight.sh`),
  the CMake glue, and our patches (`native/patches/`);
- `native/README.md`: the exact upstream versions and tarball URLs, and the steps to rebuild `plasmashell` with a
  modified Qt, KDE Framework or Plasma source (the relinking that the LGPL asks for).

The upstream sources themselves are not copied here; the exact versions and URLs are. `plasmashell` is
GPL-2.0-or-later and Qt is LGPL-3.0, so the whole program is distributed under the GPL, and its complete source is
what these scripts fetch plus the files here. The object files are not shipped: the build trees are several
gigabytes, and rebuilding from the published sources takes a few hours (`native/README.md`).

## Other notes

1. **Claude Code** (Anthropic's proprietary program) is installed only on the private development disk
   (`build-debian-image.sh dev`). The release disk is built **without** it: the `release-root` step removes it, its
   wrapper and anything of it in home directories, stops if any is left, and the `disk` step refuses a release root
   that has it. Our own `agent` program (`tools/guest/agent`, which talks to the Anthropic API with the user's own
   key) is not Anthropic's software and stays.
2. **`native/crt/nqsjlj.c`** follows Emscripten's `emscripten_setjmp.c` (MIT or UoI-NCSA, copyright The
   Emscripten authors). Whether it is a copy or a rewrite is not certain, so it keeps that notice.
3. **libwayland 1.23.1 and libxkbcommon 1.7.0** are not fetched by a script in `native/`; `native/README.md` says
   where they come from.
