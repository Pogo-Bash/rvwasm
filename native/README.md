# native/: Qt, KDE Frameworks and Plasma as native WebAssembly programs

> **Draft: not legal advice; to be reviewed by a lawyer before publication.**

The desktop shell in the release (`plasmashell`) is KDE Plasma 6.3.6 built with Qt 6.8.2 and KDE Frameworks 6.13.0
**statically**, for the linux-wasm kernel, as one WebAssembly program. This directory is everything that is ours
and needed to make it again: the toolchain, the build scripts and their order, the CMake glue and our patches.
**It does not contain the upstream sources.** It names their exact versions and where to download them, below.
For why it is built this way and how it works, see [../docs/native-wasm.md](../docs/native-wasm.md).

Licences: `plasmashell` is GPL-2.0-or-later; Qt is LGPL-3.0-only (or GPL); the KDE Frameworks are mostly LGPL.
The program is distributed under the GPL as a whole. Each patch in `patches/` is under the licence of the project
it patches, and the per-path table is in [../LICENSE.md](../LICENSE.md). The listing of components and the written
offer for source is in [../THIRD-PARTY.md](../THIRD-PARTY.md).

## What is here

| Path | What |
|---|---|
| `bin/nq-clang`, `bin/nq-clang++` | the compiler drivers: clang 18 of linux-wasm's LLVM for `wasm64-unknown-unknown`, with the flags a linux-wasm program needs, and a check of the module's imports after linking |
| `env.sh` | the paths (all under `~/rvwasm/nativeqt`: `src/`, `build/`, `host/`, `sysroot/`) |
| `toolchain.cmake`, `meson-cross.sh`, `nq-plugins.cmake` | CMake and Meson configuration for the cross build; static plugin helpers |
| `crt/` | start-up code, setjmp/longjmp support, C++ exception tag, a `__map_file` that reads instead of maps |
| `build-*.sh`, `fetch-*.sh`, `qt-prep.sh` | one script per group of packages (below) |
| `run/overnight.sh`, `run/full-build.sh` | the whole build in dependency order, resumable |
| `kf-static-deps.py`, `static-plugins.py`, `plasma-shell-only.py`, `plasma-workspace-*.cmake`, `plasma-desktop-parts/` | what makes a static, shell-only plasmashell out of plasma-workspace and plasma-desktop (these edit KDE's sources and build files, so they are GPL-2.0-or-later) |
| `wayland-noffi.py`, `sysroot-syscall.py`, `wasm-imports.py` | generated code and fixes for libwayland, musl's `syscall()` header and the import check |
| `qml-stubs/` | QML stand-ins for the parts of KPipeWire and the keyboard KCM that are not built |
| `tests/` | small Qt programs used to check the toolchain |
| `patches/nativeqt-*.diff` | our patches (below) |

## Upstream sources (exact versions)

Nothing is vendored. The scripts download these, and you can fetch them yourself.

| Package | Version | Source |
|---|---|---|
| Qt base, wayland, declarative, shadertools, svg | 6.8.2 | Debian trixie's source packages, `https://deb.debian.org/debian/pool/main/q/` : `qt6-base_6.8.2+dfsg.orig.tar.xz` with `qt6-base_6.8.2+dfsg-9+deb13u2.debian.tar.xz`, `qt6-wayland_6.8.2.orig.tar.xz` with `qt6-wayland_6.8.2-4.debian.tar.xz`, `qt6-declarative_6.8.2+dfsg.orig.tar.xz` with `qt6-declarative_6.8.2+dfsg-7.debian.tar.xz`, `qt6-shadertools_6.8.2.orig.tar.xz` with `...-3.debian.tar.xz`, `qt6-svg_6.8.2.orig.tar.xz` with `...-3.debian.tar.xz` (`fetch-qt.sh`; Debian's patches are applied first, then ours) |
| Qt 5 Compat, Image Formats, Multimedia, Positioning, Sensors | 6.8.2 | `https://download.qt.io/archive/qt/6.8/6.8.2/submodules/NAME-everywhere-src-6.8.2.tar.xz` |
| KDE Frameworks 6 (ECM, KArchive, KConfig, KIO, KService, KPackage, Kirigami, and the rest of the list in `fetch-kde.sh`) | 6.13.0 | `https://download.kde.org/stable/frameworks/6.13/NAME-6.13.0.tar.xz` |
| KDE Plasma (plasma-workspace, plasma-desktop, libplasma, libksysguard, plasma5support, kwin (one file for its D-Bus interface), kscreen, milou, kdeplasma-addons, kactivitymanagerd, breeze, ...) | 6.3.6 | `https://download.kde.org/stable/plasma/6.3.6/NAME-6.3.6.tar.xz` |
| plasma-wayland-protocols | 1.16.0 | `https://download.kde.org/stable/plasma-wayland-protocols/plasma-wayland-protocols-1.16.0.tar.xz` |
| Kirigami Add-ons | 1.7.0 | `https://download.kde.org/stable/kirigami-addons/kirigami-addons-1.7.0.tar.xz` |
| D-Bus | 1.16.2 | `https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz` |
| zlib | 1.3.1 | `https://zlib.net/fossils/zlib-1.3.1.tar.gz` |
| OpenSSL | 3.5.1 | `https://github.com/openssl/openssl/releases/download/openssl-3.5.1/openssl-3.5.1.tar.gz` |
| SQLite | 3.46.1 | `https://www.sqlite.org/2024/sqlite-autoconf-3460100.tar.gz` |
| Boost (headers) | 1.83.0 | `https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2` |
| libqrencode, libdmtx, libxml2, libxkbfile, qcoro | 4.1.1, 0.7.7, 2.12.7, 1.1.3, 0.11.0 | URLs in `fetch-kde.sh` |
| Mesa, with Debian's packaging | 25.0.7 (Debian `25.0.7-2+deb13u1`) | `https://archive.mesa3d.org/mesa-25.0.7.tar.xz`, `http://deb.debian.org/debian/pool/main/m/mesa/mesa_25.0.7-2+deb13u1.debian.tar.xz` (`fetch-mesa.sh`) |
| libdrm, wayland-protocols | 2.4.124, 1.45 | URLs in `build-mesa.sh` |
| X11 client libraries, xcb, FreeType, expat, fontconfig | the versions listed in `build-x11.sh` | `https://xorg.freedesktop.org/releases/individual`, `https://xcb.freedesktop.org/dist`, and the URLs in the script |
| libwayland | 1.23.1 | `git clone --branch 1.23.1 https://gitlab.freedesktop.org/wayland/wayland.git` into `src/wayland` (not fetched by a script here: `build-wayland.sh` expects it there, as a git checkout, and applies `patches/nativeqt-wayland.diff` only if it is unmodified) |
| libxkbcommon | 1.7.0 | `git clone --branch xkbcommon-1.7.0 https://github.com/xkbcommon/libxkbcommon.git` into `src/libxkbcommon` (the same: no fetch script here) |
| linux-wasm (musl, kernel headers, LLVM 18 and its runtimes' sources) | the commit `docker/Dockerfile.build` names | `https://github.com/joelseverin/linux-wasm`, with `patches/linux-wasm-script.diff`, `patches/musl-wasm64.diff` and the kernel patches of this repository |
| Debian trixie KDE root (QML and D-Bus interface files that `overnight.sh` copies) | | `tools/build-debian-image.sh kde-root` |

Tarball hashes are not recorded here. The versions above are the ones the scripts fetch; check them against the
upstream release signatures or Debian's `Sources` file if you need to.

## Our patches (`patches/`)

Applied by the scripts, once per source tree: Qt (`qt-prep.sh`, after Debian's patches), KDE modules
(`build-kf.sh`), and the others by their own script (`build-mesa.sh`, `build-wayland.sh`, `build-runtimes.sh`,
`run/overnight.sh` for SQLite).

| Patch | To | What it changes |
|---|---|---|
| `nativeqt-qtbase.diff` | Qt base | QProcess starts programs by cloning with a function (no `fork`), forkfd's child stack and functions, a `double-conversion` header |
| `nativeqt-qtdeclarative.diff` | Qt Declarative | V4's page allocator commits nothing and zeroes decommitted pages by hand (no `mprotect`) |
| `nativeqt-kio.diff` | KIO | static program: register the "Create New" template resource; worker start-up; the file and trash workers as static ones |
| `nativeqt-ksvg.diff` | KSvg | keeps no shared-memory pixmap cache (no MMU); renames classes that collide with Kirigami's |
| `nativeqt-libplasma.diff`, `nativeqt-plasma5support.diff` | libplasma, Plasma5Support | look up applets, containment actions and data engines by id before searching for plugin files |
| `nativeqt-kcoreaddons.diff` | KCoreAddons | plugin metadata without a plugin file |
| `nativeqt-kdeclarative.diff` | KDeclarative | `BadgeEffect` keeps its source visible under the software renderer |
| `nativeqt-kguiaddons.diff` | KGuiAddons | renames its Wayland `KeyState` to avoid a collision in one binary |
| `nativeqt-kdeplasma-addons.diff` | kdeplasma-addons | colour picker's grab widget; the comic and alternate-calendar plugins' build files |
| `nativeqt-solid.diff`, `nativeqt-kpackage.diff`, `nativeqt-knotifications.diff`, `nativeqt-kcmutils.diff`, `nativeqt-knewstuff.diff`, `nativeqt-kwidgetsaddons.diff`, `nativeqt-kirigami-addons.diff`, `nativeqt-libksysguard.diff`, `nativeqt-extra-cmake-modules.diff` | the KDE module of that name | build files only: static libraries instead of modules, options; ECM makes QML modules' own code non-optional |
| `nativeqt-mesa.diff` | Mesa | static libraries for EGL, Gallium and the GLES API; `__thread` for `thread_local`; anonymous files |
| `nativeqt-wayland.diff` | libwayland | no libffi (a switch over all signatures) |
| `nativeqt-libxkbcommon.diff` | libxkbcommon | reads files instead of mapping them shared |
| `nativeqt-sqlite.diff` | SQLite | WAL mode without a shared wal-index |
| `nativeqt-llvm18-runtimes.diff` | libc++ and libc++abi 18 | a destructor returns its argument in WebAssembly (`__cxa_init_primary_exception`) |

## Building plasmashell from these sources

You need a Linux machine (the maintainers' is an arm64 Lima VM; 12 cores and 12 GB are comfortable) with `cmake`,
`ninja`, `meson`, `python3`, `ccache`, `curl`, `git`, `patch`, a host compiler, and the toolchain below. Paths are
under `~/rvwasm` (`native/env.sh`). Reconstructed from the scripts in the order they were used; a start-to-finish
run of exactly this list, from nothing, has not been done in this form.

1. **linux-wasm and its LLVM 18** (the long part: an hour or more for LLVM): linux-wasm's own `linux-wasm.sh`
   with `patches/linux-wasm-script.diff`, kernel and musl patched as `docker/build.sh` and
   `docker/Dockerfile.build` show (the commit, the commands). The build must end up in
   `~/rvwasm/linux-wasm/workspace/install/` with `llvm/`, `musl-wasm64_nommu/` and `kernel-wasm64_nommu/`.
2. **The Debian KDE root** (for QML and interface files copied by `overnight.sh`): `tools/build-debian-image.sh kde-root`.
3. **The sysroot and sources**: `native/build-sysroot.sh`; `native/fetch-qt.sh`; `native/fetch-mesa.sh`;
   unpack libwayland and libxkbcommon as in the table.
4. **Host tools**: `native/build-host-qt.sh` (Qt's moc, rcc, qsb, qmlcachegen and the like, for the build machine).
5. **Libraries Qt needs first**: `native/build-runtimes.sh` (libc++, libc++abi, `libnqcrt.a`),
   `native/build-wayland.sh`, `native/build-mesa.sh`, then `native/build-qt.sh qtbase qtwayland`.
6. **Everything else**: `native/run/full-build.sh`. It runs `run/overnight.sh` with `NQ_FULL=1` detached and returns
   at once; the progress is in `~/rvwasm/nativeqt/logs/overnight-summary.txt`, ending in `# finished`. It builds
   the remaining libraries, Qt again with D-Bus, SQL, TLS and xcb, the host KDE tools, 100 steps of Frameworks and
   Plasma, and links `plasmashell` (about 137 MB) into `~/rvwasm/nativeqt/sysroot/usr/bin/plasmashell`. A finished
   step is skipped when it is run again; a failed one is tried again.
7. **Into the release**: `tools/nq-bundle.sh` takes that `plasmashell` and the `share/` files and
   `docker/release.sh bundle root disk ...` puts it in the image.

## Rebuilding with a modified Qt, KDE Framework or Plasma (the relinking the LGPL asks for)

`plasmashell` is one static program. To change a library in it, change the source and link the program again from
the same build tree. Because `plasmashell` itself is GPL and its whole source is published (this directory plus the
upstream sources above), you can rebuild any part; the steps are:

1. Edit the source under `~/rvwasm/nativeqt/src/NAME` (for Qt: `qtbase`, `qtdeclarative`, ...), or put your own
   diff in `patches/nativeqt-NAME.diff` before the first build.
2. **Qt:** `NQ_REDO=1 native/build-qt.sh NAME` rebuilds that module (incrementally) and installs its static
   libraries into the sysroot. **A KDE module:** `NQ_REDO=1 NQ_KF_EXTRA="..." native/build-kf.sh NAME` (the
   options of each module are in `run/overnight.sh`; copy the line for the module).
3. Link again. If you only changed a source file, not a header or a class layout, relinking is enough:
   `ninja -C ~/rvwasm/nativeqt/build/kf-plasma-workspace bin/plasmashell`, then copy the result to
   `~/rvwasm/nativeqt/sysroot/usr/bin/plasmashell`. This is the last step of `run/overnight.sh`
   (`plasmashell-relink`). It relinks because CMake names each static library by its path in the link rule; if you
   doubt that it did, `ninja -d explain` says why.
4. If a header or class layout changed, everything that includes it must be compiled again: remove the markers
   `rm ~/rvwasm/nativeqt/build/kf-*/.nq-installed` and run `native/run/full-build.sh` again. ninja recompiles only
   what depends on the change.
5. Put the result in the image as in step 7 above, and try it with `desktop wayland plasma-native` in the machine.

**Object files are not shipped.** The build trees and the sysroot are several gigabytes, depend on the paths of
the machine that made them, and need the same toolchain to be useful. They are not a good thing to put in a git
repository or a download. What a recipient needs to relink is the program's source and these scripts, which are
here; the build takes several hours on a 12-core machine (the stage tables in `docs/native-wasm.md` have the
timings of the first stages).
