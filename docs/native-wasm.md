# Native Qt/KDE programs: compiled straight to WebAssembly

This is the working log of the native build. Names such as `build/nm/...`, `build/shots-*` and the branch names
refer to the maintainers' development trees and are not part of this repository. The build scripts, the patches and
how to rebuild are in [../native/README.md](../native/README.md).

Goal: KDE Plasma's shell (plasmashell) and Qt/KDE programs run on the machine
as native linux-wasm programs (as rv, hostnode and busybox are), not as
Debian's riscv64 binaries under rv's translator.

Everything here is built in the build VM with `native/*.sh` (they source
`native/env.sh`); sources, builds and the sysroot live in the VM under
`~/rvwasm/nativeqt/` (`src/`, `build/`, `sysroot/`, `host/`, `logs/`), the
worktree is copied to `~/wt/nativeqt` by `build/sync.sh`.

## How a native program works on linux-wasm (what the toolchain has to make)

- One program is one Wasm module with a `dylink.0` section first: binfmt_wasm
  (kernel `fs/binfmt_wasm.c`) reads its `meminfo` (memory size and alignment,
  table size) and nothing else, maps the whole file and allocates the data
  area and a 256 KiB stack; the runtime (`linux-worker.js`) compiles the
  bytes and instantiates the module with `env.memory` (the machine's one
  shared 64-bit memory), `__memory_base` (the data area), `__table_base` (0),
  `__stack_pointer`, `__indirect_function_table` (a table of its own) and the
  system call imports `__wasm_syscall_0..6`.
- Every task is a Web Worker. A thread (`clone`) is the same module
  instantiated again in another Worker with the same `__memory_base`; it gets
  its own table, filled the same way by the element segments, so function
  pointers mean the same in every thread. `__wasm_init_memory` (data
  segments) runs only for the first instance; the runtime calls
  `__wasm_apply_data_relocs`, then `__libc_clone_callback` or `_start`.
- What the runtime calls in a program: `_start`, `__set_tls_base`,
  `__libc_clone_callback`, `__wasm_apply_data_relocs`, `__wasm_call_ctors`,
  `__libc_handle_signal`. Nothing else needs to be exported.
- libc is linux-wasm's musl (static). Its own `setjmp` returns 0 and its
  `longjmp` aborts (musl `src/setjmp/wasm`); our programs do not use them
  (Wasm SjLj, below).
- No MMU: no `fork` (vfork/posix_spawn only), `mmap` of files is a copy
  except ramfs/shmem files truncated up from 0 (see wlserver/README.md).

## The toolchain (stage 0)

`native/bin/nq-clang` / `nq-clang++` (clang 18 of linux-wasm's LLVM,
without the Python wrapper in between):

- `--target=wasm64-unknown-unknown`, `-D__linux__`, atomics, bulk memory,
  `-fPIC -pthread`, Wasm exception handling (`-fwasm-exceptions`, and
  `__USING_WASM_EXCEPTIONS__`, which clang 18 only defines for Emscripten).
- C++: libc++/libc++abi 18 (the toolchain's own sources, static, threads,
  exceptions) built by `native/build-runtimes.sh`, with one fix
  (`native/patches/nativeqt-llvm18-runtimes.diff`: in Wasm a destructor returns its
  argument; LLVM 18 has that for `__cxa_throw` but not for
  `__cxa_init_primary_exception`, fixed in LLVM 19). libunwind for Wasm EH is
  only `Unwind-wasm.c` (as Emscripten builds it).
- Linking makes a position independent *executable* (`wasm-ld
  --experimental-pic -pie`), not `-shared` as tools/wasm-cc does: with
  `-shared`, wasm-ld imports every weak definition (operator new, libc++'s
  template instantiations) and the C++ exception tag from the host, which
  has none of them. With `-pie` everything is bound in the module; the tag
  is defined by `crt/cpp_exception.S`.
- This wasm-ld imports undefined symbols instead of failing (in every mode
  we tried), so the wrapper reads the import section after linking and fails
  on anything the runtime does not give (`native/wasm-imports.py`). That
  keeps configure checks honest.
- Only the runtime's entry points are exported (V8 takes at most 100,000
  exports), and `--gc-sections` is on.
- `crt/nqcrt.c`: clang names a C++ `int main()` `__main_void`; linux-wasm's
  crt1 calls `__main_argc_argv`.
- CMake: `native/toolchain.cmake`; Meson: `native/meson-cross.sh`
  writes the cross file.
- Static constructors run inside musl's `__libc_start_main`, after
  `__init_libc` (`crt/nqstart.c` defines `__libc_start_init`, and
  `__wasm_call_ctors` is not exported). The runtime calls an exported
  `__wasm_call_ctors` *before* `_start`, when musl has no page size yet:
  every malloc in a constructor failed (EINVAL from mmap), and Qt's static
  plugin registration made that a std::bad_alloc before main.
- setjmp/longjmp: compiled with `-mllvm -wasm-enable-sjlj` (LLVM's Wasm SjLj
  lowering on Wasm exception handling); `crt/nqsjlj.c` has the functions it
  calls (saveSetjmp, testSetjmp, __wasm_longjmp, as Emscripten's compiler-rt
  for LLVM 18) and `crt/sjlj_tag.S` the tag. FreeType longjmps on an
  ordinary path (a band too big for its cell pool), so before this the first
  text a Qt program drew aborted it. `tests/sjlj` checks it.
- Long double compares (`__eqtf2` and the rest) come from `libnqcrt.a`, built
  from compiler-rt's own source with `int` results: compiler-rt 18 returns
  `long` there on LP64 targets other than AArch64, an i64 on wasm64, where
  LLVM's calls expect an i32. The mismatch is a trap the first time printf
  formats a double (musl's printf_core compares long doubles). The same
  mismatch is in every program linked with linux-wasm's own compiler-rt
  (tools/wasm-cc), as far as we can tell: busybox's `printf %f` printed
  nothing under node-machine.
- Weak undefined functions become imports in a PIE link (`GOT.func.*`), so
  the import check fails on them; fixed where they came from (libc++abi's
  `__cxa_thread_atexit_impl`, Qt's `__interceptor_vfork`, Mesa's C++
  `thread_local` wrappers).
- Debugging: link with `-Wl,--wrap=__cxa_throw -Wl,--wrap=_Unwind_RaiseException
  -Wl,--wrap=malloc` and run with `NQ_THROW_TRACE=1` to hear of every throw
  and every failed malloc (`crt/nqcrt.c`). The runtime names the functions of
  a crash's stack (the name section), e.g. `signature_mismatch:__eqtf2`.
- `syscall(n, ...)`: linux-wasm's musl reads every argument with
  `va_arg(ap, long)`, but Wasm's C varargs keep an `int` 4 bytes (x86-64
  would have widened it in a register). `syscall(SYS_futex, addr, FUTEX_WAIT,
  val, ...)` gave the kernel op and val packed into one argument and the rest
  shifted: Qt's QMutex (its own futex calls) waited on the wrong value and its
  wakes woke nobody; a Qt Quick program spun at 100 % CPU, drew a frame every
  few seconds and hung when the raster engine's thread pool was contended.
  The sysroot's `<unistd.h>` (`sysroot-syscall.py`) now casts every argument
  to long. Every linux-wasm program built from C that calls `syscall()` with
  int arguments has the same problem (libc++'s futex waits, Mesa's, Qt's).
- The main stack is 8 MiB (`crt/nqstart.c` maps it and moves the stack
  pointer before the constructors; the main thread's `struct pthread` says
  so, for `pthread_getattr_np`), threads get 1 MiB. binfmt_wasm's stack is
  256 KiB with nothing below it but whatever was allocated there, kernel
  memory included; code sized for 8 MiB stacks (Qt's QML engine checks its
  recursion against RLIMIT_STACK) runs off it. `NQ_MAIN_STACK_KB`,
  `NQ_THREAD_STACK_KB` change the sizes.
- Runtime item 21: a new thread no longer calls `__wasm_apply_data_relocs`
  (it put back the first value of every pointer in initialised data that the
  program had changed since it started).
- QML (qtdeclarative): V4's page allocator (WTF OSAllocatorPosix) reserves
  anonymous read-write memory and commits nothing on wasm (no mprotect
  without an MMU; it crashed at the engine's first allocation); decommitted
  pages are zeroed by hand. `native/patches/nativeqt-qtdeclarative.diff`.
- QProcess always uses vfork on wasm (no fork without an MMU); forkfd's own
  clone path is not tried yet.

Runtime change (tools/patch-runtime.py item 20): a program's function table
gets the size its import asks for (it was always 4096 slots; Qt needs tens of
thousands). Its reader of the import section skipped each name's length byte
short (`p += leb()` reads `p` before `leb()` moves it), so it found no table
in a program with imports; fixed.

## Linking: one static module per program (decision)

Decision: **static linking**. Each program (a Qt test, then plasmashell) is
one module with Qt, the KDE Frameworks it uses, their plugins and QML
modules linked in. Shared libraries are not implemented now.

Why, with evidence:

- The loader we have is binfmt_wasm + linux-worker.js: one module per
  process, and every thread instantiates it again in its own Worker with its
  own table. Shared libraries would need a dynamic loader in the runtime that
  instantiates each library in *every* Worker of the process at the same
  memory and table bases, resolves `GOT.mem`/`GOT.func` imports, and makes a
  `dlopen` in one thread visible in all others before they call into it
  (Emscripten's "dlsync" problem). It also needs the kernel to place library
  data. That is weeks of runtime/kernel work before the first Qt pixel.
- The toolchain's wasm-ld 18 `-shared` output is not usable as is (weak
  definitions and the exception tag become imports, above).
- Qt supports static builds with static plugins (Qt for iOS and for
  Emscripten are static), including QML modules (`qt_import_qml_plugins`);
  KF6 has static plugin support (`kcoreaddons_target_static_plugins`,
  KPluginFactory static plugins).
- V8's limits are far away: module size 1 GiB, 1,000,000 functions, 7.6 MB per
  function body, 100,000 exports (hence the short export list), table init
  entries 10,000,000. (Sizes measured per stage below.)
- Costs we accept: every program carries its own copy of Qt (disk, and V8
  compiles each program separately; threads of one program share the
  compiled code through V8's NativeModuleCache, which is keyed by the
  bytes). exec maps the whole file and the runtime copies the bytes to
  compile them (per thread too); for a ~100 MB plasmashell that is a copy per
  thread start, to be replaced by handing the compiled `WebAssembly.Module`
  to new Workers if it shows.

Known risk to watch: the runtime calls `__wasm_apply_data_relocs` on every
thread's instantiation, which writes the initial value of every pointer in
initialised data again; a program that changed such a pointer before
starting a thread gets it back. Not seen yet.

## Stages

| Stage | What | Status | Numbers |
|---|---|---|---|
| a | C++ hello: exceptions (deep unwind, int, in threads), RTTI, 4 std::threads, thread_local | runs (node-machine) | 734 KB module; whole run 0.36-0.47 s under Node incl. 4 thread Workers |
| b | Qt 6.8.2 base + Wayland plugin + Widgets window | runs (node-machine, wlserver, PNG of the window) | below |
| c | Mesa EGL/GLES virgl over vtest | client side runs (node-machine: softpipe renders; virgl through gpuserver to the stub executor) | below |
| d | Qt Quick app | | |
| e | KF6 + plasmashell | plasmashell 6.3.6 links (71.5 MB module, -O1), not run yet | below |

### Stage b: the QWidget test, native against riscv64 under rv

`tests/qwidget` (a label, a QPainter canvas of 24 antialiased ellipses and
text, a button), the same source built for riscv64 against Debian trixie's
Qt 6.8.2 (`build-rvref.sh`), both on wlserver under node-machine
(`build/q3.sh`: the server already up, each program started twice; Node
22's V8, not Chrome; the host shared with other work, so +-20%):

| | native wasm | riscv64 under rv |
|---|---|---|
| module / binary | 23 MB (Qt, plugins, fonts code in it) | 34 KB + Debian's Qt libraries |
| launch to first frame, 1st start | 0.53 s | 3.1 s |
| launch to first frame, 2nd start | 0.20 s | 1.05 s |
| idle CPU over 20 s (window up) | 0.4-0.5 % | 1.6-2.8 % |
| animating (repaint as fast as it can) | 67-74 fps at 11-21 % CPU | 26-45 fps at 90-97 % CPU |
| mappings of the process (/proc/PID/maps) | 27 MiB | 84-86 MiB |

(Launch to first frame of the riscv64 program was 38 s the first time the
machine ran it after boot: rv's translation of Qt's libraries; the numbers
above are with that done once.)

### Stage c: Mesa

Mesa 25.0.7 as Debian's, `-Dgallium-drivers=softpipe,virgl -Dplatforms=wayland
-Dllvm=disabled`, static (`native/patches/nativeqt-mesa.diff`: Meson's `library()`
for libEGL/libgallium/libGLESv2; libEGL takes the static pipe loader;
`__thread` instead of C++ `thread_local`), `nq-deps/` files telling
bin/nq-clang what -lEGL and -lGLESv2 need after them. `tests/egl`
(surfaceless EGL, GLES 2 triangle, glReadPixels) under node-machine:
softpipe draws it right (centre pixel 51,204,77 = the colour asked);
`GALLIUM_DRIVER=virpipe` through gpuserver with the stub executor
(gpu/tools/gpu-stub.js): `virgl (LLVMPIPE ...)`, GL ES 3.0, 30 frames, 30
SUBMITs and 30 TRANSFER_GETs reached the executor. Real pixels from the GPU
need the page (a run in the browser).

### Stage d: Qt Quick (status 2026-10-10 evening)

`tests/qquick` (12 rotating rounded rectangles and a text in QML, no JIT) under node-machine on wlserver,
after the syscall/stack/relocation fixes: Qt Quick's software renderer runs, 141 and 153 frames in 15 s
(9.7 and 10.3 fps: wl-dump acknowledged frames every 100 ms then, so that is its pace, not the program's)
at 9.7-10.3 % CPU, first frame 220-320 ms after main, 93 MiB of mappings. The riscv64 build of the same
source under rv: 87 frames in 9.6 s (9.1 fps, the same pace) at 47.8 % CPU, first frame 3.7 s after main,
133 MiB. With OpenGL through Mesa's softpipe on Wayland (EGL's swrast platform) the kernel crashes
(indirect calls through garbage in the kernel's own objects: a socket's sendmsg, hvc, fbcon): something in
that path writes over kernel memory. Not found yet; build/memwatch.js (a hook that watches low memory) saw
nothing in a software run.

### Stage e: the build to plasmashell (2026-10-10)

`native/run/overnight.sh` (start with `run/start-overnight.sh`; summary in
`~/rvwasm/nativeqt/logs/overnight-summary.txt`) builds, at -O1, in order: libdbus, SQLite, OpenSSL 3.5.1,
zlib, the X11 client libraries (`build-x11.sh`, as Debian builds Qt, KWindowSystem and Plasma with them),
Qt again with QtDBus, QtSql, TLS and xcb, ECM, the host tools (Qt's qdbusxml2cpp, kconfig_compiler,
kpackagetool6, kcmutils' tools), 33 KDE Frameworks 6.13, kwayland, layer-shell-qt, plasma-activities,
libplasma, and plasma-workspace cut down to libkworkspace + shell (`plasma-shell-only.py`). Last run: 62
modules ok, `bin/plasmashell` linked (71.5 MB). What it took, beyond options: add_library(MODULE/SHARED)
made static (toolchain.cmake), Qt's versionless DBus/shader macros (ECMQmlModule loads Qt without them),
static packages that find their private dependencies (`kf-static-deps.py`), and small patches
(`native/patches/nativeqt-{solid,kio,knotifications,kpackage,kcmutils,libplasma}.diff`). Not yet: its static
plugins (QML modules, KPackage structures, applets, the Wayland integrations) are not imported, so it will
not get far when run; nothing of stage e has run.

### Stage e: plasmashell runs (2026-10-10 night)

The native plasmashell (110 MB module) starts on wlserver under node-machine with Qt Quick's software
renderer and draws Debian's Plasma desktop (the shell package, the folder-view desktop containment and the
image wallpaper, all from the disk's /usr/share/plasma) as the riscv64 one does. What it took: every static
plugin linked and registered (`static-plugins.py`; `plasma-shell-only.py` adds this project's plugins, the
QML import scan over a copy of Debian's Plasma QML, the containment layout manager, the image wallpaper,
the context-menu/paste actions and libtaskmanager; `plasma-desktop-parts/` the folder view and activity
switcher), QML modules' type registrations linked, ECM's own-code QML plugins made non-optional
(`native/patches/nativeqt-extra-cmake-modules.diff`: in a static program the module is registered before Qt would
load the plugin, which then never ran), ksvg's copies of Kirigami's classes renamed (`native/patches/nativeqt-ksvg.diff`).
Found by tracing the riscv64 plasmashell (QT_DEBUG_PLUGINS, QML_IMPORT_TRACE, WAYLAND_DEBUG):
`native/tests/run-ps.sh`, `native/tests/ps.sh`.

Measured under node-machine, both on the same wlserver, `daily` disk, software Qt Quick, 180 s each:

| | native wasm | riscv64 under rv |
|---|---|---|
| launch to the desktop's first frame | 6.4 s | 22.2 s |
| CPU to get there and settle (whole run) | 3.7 s | 20.5 s |
| CPU over the last 30 s (desktop up, idle) | 0 | 0 |
| mappings of the process at the end | 612 MiB | 517 MiB |

(The native mappings include the whole 110 MB module, which binfmt_wasm maps, and on this kernel every
mapping is rounded up to a power of two of pages; not looked into further yet.)

Still missing in the native one: KIO's desktop:/ worker (the folder view shows "Unknown protocol
'desktop'": KIO workers are separate programs, none built), a Kirigami platform plugin for the "Fusion"
style (qqc2-desktop-style, not built), kglobalaccel and KWin on the session bus (the run had a bus of its own).

### The native plasmashell as the shell (kdeproto's wlserver, 2026-10-10 late)

`desktop wayland plasma-native` (branch kdeproto, merged) with the native plasmashell first on the PATH,
Plasma's default layout (no appletsrc), Konsole started after 90 s (`native/tests/pn.sh`, `run-pn.sh`):
the desktop (a background layer surface, 1024x768) and the panel (a layer surface at 0,722 1024x46,
docked at the bottom, Breeze's floating panel) both presented 13.6 s after the launch of `desktop`
(server start included), plasmashell then idle (0 CPU over 30 s; 3.8 s of CPU in all; 633 MiB of
mappings); Konsole's window placed above the panel's exclusive zone. The panel's applets show their
error placeholders: their C++ halves are not built yet: kickoff (org.kde.plasma.private.kicker,
plasma-desktop), pager (org.kde.plasma.private.pager, plasma-desktop), icontasks
(org.kde.plasma.workspace.dbus from plasma-workspace, and the task manager's plugin, which needs
libksysguard's ProcessCore and libnotificationmanager), digital clock (org.kde.plasma.private.digitalclock
and the calendar), show desktop (its PeekController), the system tray (a containment plugin; needs
Plasma5Support). So no window list and no task to click yet. The riscv64 plasmashell under rv set the
panel up but had not drawn it after 6 minutes (kdeproto's run).

### The panel's applets (2026-10-11)

The applets' C++ halves, linked into plasmashell the way the folder view is (static plugins; old-style QML
plugins with `nq_old_qml_plugin`, native/nq-plugins.cmake): libksysguard's Formatter and ProcessCore
(plasma-desktop-parts; libksysguard's compiler-settings level is 5.82, so none of the 5.85 Qt definitions;
`-fexperimental-library` for libc++ 18's jthread/stop_token), libnotificationmanager, libtaskmanager,
the task manager's plugin, org.kde.plasma.workspace.dbus, calendar, digital clock (without ICU: its
time-zone picker shows zone ids), kicker (Kickoff), pager, show desktop, the triangle mouse filter,
libkmpris, Plasma5Support (KSysGuard optional: no device-notifications engine) with the time engine,
kirigami-addons 1.7.0 (without its stateful application), the system tray (applet, container,
libdbusmenuqt). Collisions in one static binary, renamed by compile definitions: kicker's `WindowSystem`
(KWindowSystem's Wayland plugin has one), libdbusmenuqt's DBusMenu types (KStatusNotifierItem has them).
Two lookups by path that never find a static plugin, patched to look by id first: Plasma5Support's data
engines (native/patches/nativeqt-plasma5support.diff) and libplasma's applets and containment actions
(native/patches/nativeqt-libplasma.diff; this is why the system tray's container, a C++ applet, did not load).

Run (`pn.sh`, default layout, Konsole at 120 s): the panel draws Kickoff, the pager, the task manager
(Plasma's pinned launchers and Konsole's task), the system tray, the digital clock (time and date) and
show desktop. The click test passes: a click on Konsole's task minimizes it (wlserver flags 35 -> 97,
focus off), a second click restores and activates it (flags 35, focus 4). plasmashell: 4.7-5.3 s of CPU
in all, 0 CPU over the last 30 s, 648-662 MiB of mappings (633 MiB before the applets); the binary is
122 MB. Still missing: the tray's own applets show placeholders (org.kde.plasma.private.notifications,
.clipboard, .devicenotifier, workspace.keyboardlayout / kcm_keyboard, workspace.components,
org.kde.pipewire.monitor), the task tooltips' window thumbnails (org.kde.pipewire), Kickoff's
start-here-kde-symbolic icon (drawn as a fallback), and the activity database (no kactivitymanagerd).

### The first browser run and its fixes (2026-10-11, branch plasmanative)

- Launching from Kickoff killed plasmashell: KIO starts programs with QProcess; on linux-wasm fork() fails,
  vfork() aborts, clone() with a function and posix_spawn work (native/tests/launch). Qt's child runs
  forkfd's clone() child, but its functions were noexcept, and exec unwinds the old program with a
  JavaScript exception: std::terminate ("libc++abi: terminating"). The child's functions are not noexcept on
  wasm, startDetached clones with a function instead of vfork(), forkfd's child stack is 128 KiB.
- xkbcommon mapped its files MAP_SHARED, which fails for a disk file without an MMU: no Compose table. It
  reads them on wasm (native/patches/nativeqt-libxkbcommon.diff).
- Kickoff's search found no applications: the services runner is built in. Typed text reached the field.
- The tray: the notifications applet (and KQuickCharts) and the device notifier's plugin are built; the
  keyboard layout, input method, clipboard (Klipper needs Prison) and camera indicator (KPipeWire) applets
  are left out by the tray (NQ_TRAY_UNAVAILABLE) instead of showing errors. The six "QPainter::begin: Paint
  device returned engine == 0" lines were the six failed applets' and went with them.
- wlserver's text input no longer sends a leave to a surface its client has unmapped (Qt: "Got leave event
  for surface 0x0" when Kickoff closed).
- Kickoff's "Cannot open: .../start-here-kde-symbolic" is Kickoff's own Image fallback trying the icon name
  as a file; the button shows the icon.

- A colour scheme (Breeze Dark) did not reach the native panel. Two causes: (1) no Qt platform theme in the
  static plasmashell, so Qt's palette never followed kdeglobals and a restarted shell stayed light: Plasma's
  (plasma-integration's KDEPlasmaPlatformTheme6) is built in now, and takes KGlobalSettings' notifyChange
  from System Settings (seen on the bus: notifyChange 0, PaletteChanged); (2) after the palette changed
  live, ksvg drew the panel from its pixmap cache (348 cache hits, no new renders after the change): its
  KImageCache is shared memory that falls back to anonymous memory without an MMU ("Couldn't establish file
  backed memory mapping", "Unable to unmap shared memory segment"), and was not emptied. ksvg keeps no
  pixmap cache on wasm (native/patches/nativeqt-ksvg.diff). Under Node: light at start, dark within 40 s of
  plasma-apply-lookandfeel -a org.kde.breezedark.desktop, dark again after a restart.

### What Debian's plasmashell loads, natively (2026-10-11, after a Catppuccin theme run)

Versions match Debian trixie where it matters (plasma-workspace, plasma-desktop, plasma-integration 6.3.6,
KF6 6.13, Qt 6.8.2; Debian's libplasma is 6.3.5, libksysguard 6.3.5 and plasma5support 6.3.4, ours 6.3.6: the
QML on the disk is Debian's, the C++ its own). The errors were missing C++, not a version mismatch:
panelspacer's "Cannot read property 'width' of undefined" is its C++ applet (Plasmoid.containmentItem) not
built. Built now: plasma-workspace's applets panelspacer, icon, appmenu, keyboardindicator, mediacontroller,
systemmonitor; its modules sessions, shell, lookandfeel, battery, workspace.components, workspace.keyboardlayout
(KWin's virtual keyboard interface made from KWin's source); its containment actions (switch desktop/activity/
window, application launcher) and the executable and application jobs engines; the runners locations, places,
sessions, shell, kill, web shortcuts, recent documents; libksysguard whole (sensors, faces, systemstats);
KNewStuff (+ Attica, KTextWidgets), KHolidays; qtimageformats (WebP, TGA, WBMP, ICNS, TIFF); plasma-desktop's
trash and kimpanel plugins; KIO's trash worker as a static worker; a QML stand-in for KPipeWire's
org.kde.pipewire (the import works, no thumbnails). Collisions renamed: KGuiAddons' Wayland KeyState, the
application launcher action's AppLauncher. Static plugin fixes: kimpanel's old-style plugin needs its URI (a
static plugin without one breaks every QML import), K_PLUGIN_CLASS plugins registered by their install
directory (libksysguard's sensor face package structure), no plugin files tried (KCoreAddons, libplasma,
Plasma5Support), AUTORCC without zstd. Under Node, a top panel made by a layout script (app menu, two spacers,
trash, media player, CPU monitor, lock/logout) and a WebP wallpaper show; the remaining messages are Debian's
own (no kactivitymanagerd, no session backend, no desktop toolbox package in Debian, no plasma-pa).
Not built, and why: plasma-pa (org.kde.plasma.private.volume: not on Debian's disk either), KPipeWire itself
(libpipewire loads SPA plugins at run time), Klipper (KF6 Prison and its QR libraries), kcm_keyboard
(plasma-desktop's keyboard KCM: X11 xkb libraries), libcolorcorrect (Qt Positioning), KNotifyConfig (Qt
Multimedia), Baloo (LMDB and its indexer), kdeplasma-addons (not on Debian's disk). The system monitor widget
loads with no face data (no ksystemstats on this system, nor on Debian's).

### The full build (run/full-build.sh, 2026-10-11 midday)

`run/full-build.sh` (overnight.sh with NQ_FULL=1): 100 steps ok; plasmashell 137 MB with kdeplasma-addons,
Milou, KScreen's applet, Prison and Klipper, KNotifyConfig, KImageFormats, BluezQt, and Qt Positioning,
Multimedia and Sensors without their hardware plugins. What it installs in share/plasma that Debian's disk
has not (147 files: kdeplasma-addons' widgets and wallpapers, KScreen's applet) goes with it as nq-share.tgz
(get.sh unpacks it into /run/nq/share, `desktop
wayland plasma-native` puts that first in XDG_DATA_DIRS).

Under Node (branch plasmanative's build/nm/extra/nq/full2.sh, full3.sh): a top panel by a layout script
(the store's Start Next Menu, a spacer, Milou, binary clock, colour picker, system tray) and Fifteen Puzzle,
Notes, Timer, Calculator and Fuzzy Clock on the desktop. All drawn, no error boxes; the puzzle's popup plays;
the tray shows Klipper's clipboard and KScreen's Display Configuration; Milou's search finds Konsole within
0.3 s and Enter starts it. Fixed: Klipper kept no history ("Failed to load history resource"): its
history3.sqlite is in WAL mode and a connection that writes needs the *-shm file mapped shared, which
fails without an MMU. A writing connection on wasm now takes a WAL database the way locking_mode=EXCLUSIVE
does (an exclusive lock on the file, the wal-index in heap memory; native/patches/nativeqt-sqlite.diff); another
process gets SQLITE_BUSY meanwhile. Read-only connections keep the earlier path (kactivitymanagerd's
database, which rv writes).

Seen and left, because they are the widgets' or libplasma's own code (the QML is Debian's or upstream's):
- Start Next Menu (Catppuccin's launcher): typing in its Favorites view shows nothing. Its search handler
  writes `showFavorites` unqualified (a property of the Dialog `root`, not in the handler's scope): "Invalid
  write to global property". Debian's riscv64 Qt says the same (plasmawindowed under rv, a copy of the
  widget that puts text in the field from a timer) and also keeps the favourites grid, which is shown while
  `rootItem.showApps` is 0, which typing in the field never changes. The widget's bug.
- Milou in a panel: the popup stays one line tall (the results are there). The popup (libplasma's
  AppletPopup) takes its size from the full representation's Layout hints and implicit size; Milou's
  ColumnLayout has a ListView (no implicit height) and sets only its own minimumHeight/maximumHeight
  properties, which go to the PlasmoidItem. Not compared on Debian's plasmashell: under node-machine it
  ends in ERR_WORKER_OUT_OF_MEMORY before the panel. (Fixed in round 4, below, by a copy of the QML.)
- 12 x "QObject::connect(QObject, Plasma::Applet): invalid nullptr parameter": Timer's predefined timers are
  spliced into Plasmoid.contextualActions, which grows the list property with null entries first.
- Binary Clock in a 40 px panel: its dot size goes negative and the dots are hidden. Fifteen Puzzle below
  about 270 px shows its compact representation, whose icon (fifteenpuzzle) is not in Breeze.
- Klipper sees what is set over D-Bus (setClipboardContents, getClipboardContents read back); the window
  server has no data-control protocol ("Could not init WaylandClipboard, falling back to QtClipboard"), so
  copies in other windows reach it only while plasmashell has the focus, if at all.

### The fourth browser run and its fixes (2026-10-11 afternoon, branch plasmanative)

Six findings, all seen again under Node (plasmanative's build/nm/extra/nq/r4*.sh; shots in build/shots-r4*):

1. **Right-click on the desktop did nothing.** libplasma looks for a containment action by its plugin id,
   which is its file name (`plasma/containmentactions/org.kde.contextmenu`), and the static registry had
   it under the CMake target name (`plasma_containmentactions_contextmenu`): "Cannot load ... contextmenu".
   `plasma-workspace-end.cmake` registers each plugin under its OUTPUT_NAME when it has one. BTN_RIGHT was
   never the problem: the page's runtime maps button 2 to 0x111 and wlserver's seat forwards it; under Node
   a right click at the desktop opens the menu (Desktop and Wallpaper, Create New, Icons, Paste, Undo, Enter
   Edit Mode), and Enter Edit Mode shows Add or Manage Widgets.
2. **"Could not read file applications:org.kde.discover.desktop" and a blank launcher.** Stock
   libtaskmanager keeps a launcher whose application is missing (icon "unknown", and a click runs
   ApplicationLauncherJob, which fails with that message): Plasma on any system without Discover does the
   same; the default pin list is in the task manager's main.xml and assumes Discover is installed. Debian's
   disk has none. `tools/x/nq-share/share/plasma/layout-templates/org.kde.plasma.desktop.defaultPanel`
   (found before Debian's: /run/nq/share leads XDG_DATA_DIRS) writes the pin list itself and leaves out
   what `applicationPath()` does not find. Written as an array: Plasma's `writeConfig` hands a string to
   the config scheme as a one-item list, so a comma-joined string was ONE launcher (a blank icon, nothing
   else) until the next start. An appletsrc written by an earlier run keeps its pins (remove the
   launcher or delete ~/.config/plasma-org.kde.plasma.desktop-appletsrc).
3. **Serif fonts.** Qt was built without fontconfig (and with its bundled FreeType): its own font database
   has no family matching or substitution (that is fontconfig's), so "Noto Sans" was not found and the
   fallback was a serif face. Qt now links fontconfig and the
   sysroot's FreeType (`build-qt.sh`: FEATURE_fontconfig=ON, FEATURE_system_freetype=ON); fontconfig was
   built with `--sysconfdir=/etc --localstatedir=/var`, so it reads Debian's /etc/fonts and caches, and
   plasma-integration asks for Noto Sans 10 as Debian's programs do (qt.text.font.match: family Noto Sans).
4. **Fuzzy Clock showed UTC.** Qt's time zone code (QTimeZone: the digital clock) reads /usr/share/zoneinfo
   by itself, but QML's JavaScript Date and QDateTime::currentDateTime() go through the C library's
   localtime(), and musl reads the zone file with `__map_file`, which maps it MAP_SHARED: that fails for a
   file on a disk without an MMU, and a failed read leaves UTC. `crt/nqcrt.c` has a `__map_file` that reads
   the file into anonymous memory instead (libnqcrt.a comes before libc.a). `tests/tz` checks TZ=America/
   Chicago, TZ=UTC and no TZ: libc, QDateTime, QTimeZone and JS Date agree.
5. **Klipper's current item hid its text.** The delegate draws the current item's text through an
   OpacityMask (Qt5Compat.GraphicalEffects, a ShaderEffect, so the label fades out behind the item's tool
   buttons) and hides the plain label. Qt Quick's software renderer, which plasmashell runs with here,
   draws no ShaderEffect: a highlight with nothing on it. `plasma-shell-only.py` patches the delegate so
   that with the software renderer (GraphicsInfo.api === Software) the label stays and is narrowed by the
   tool buttons instead.
6. **Milou's popup one line tall.** CompactApplet.qml's AppletPopup takes its size from the *full
   representation item's* Layout attached properties (and its implicit size); Milou's ColumnLayout has a
   ListView, which has no implicit height, and puts its minimumHeight/maximumHeight (the content height)
   only on the PlasmoidItem, which nothing reads for the popup. Same QML and libplasma as upstream, so
   Debian's plasmashell has this too (not run: rv's Plasma does not get that far). The share bundle's copy
   of org.kde.milou (`tools/x/nq-share/share/plasma/plasmoids/org.kde.milou`, Debian's QML plus three Layout
   lines on the ColumnLayout) makes the popup as tall as the field and its results.

Checked under Node after the relink (plasmanative's build/nm/extra/nq/r4q.sh, r4n.sh, r4z.sh; shots in
build/shots-r4q, r4n2): the desktop's right-click menu and Enter Edit Mode, the panel with three pinned
launchers and no notification, Noto Sans throughout, Fuzzy Clock "Quarter past one" at 13:14 with the digital
clock (TZ=America/Chicago), Klipper's hovered entry with its text, Milou's popup with the Konsole result below
the field. `tests/tz` (r4z.sh) agrees across libc, QDateTime, QTimeZone and JS Date for America/Chicago, UTC,
Asia/Kolkata and no TZ (/etc/localtime -> Etc/UTC).

### The fifth browser run and its fixes (2026-10-11 evening, branch plasmanative)

Five findings, all seen and checked under Node (plasmanative's build/nm/extra/nq/r5*.sh; shots in build/shots-r5*).
Debian's riscv64 `ksystemstats` package runs under rv as the sensors' D-Bus service. Fixed in this round: the
"Create New" submenu, the widget explorer's previews, the system monitor widgets, the camera indicator and
keyboard layout widgets (load errors), and Fifteen Puzzle's icon.

1. **Right-click -> Create New did not open.** Not the window server: the Icons submenu of the same menu opens
   (a nested xdg_popup with the parent's xdg_surface, a positioner and a grab; shot r5c), and the client never asked
   for a popup for Create New (no `get_popup` in the trace). The submenu was empty. KIO's KNewFileMenu builds it
   from a template resource compiled into KF6KIOFileWidgets (`new_file_templates/templates.qrc`, prefix
   `/kio5/newfile-templates`: Folder, Text File, Empty File, HTML File, three links). In a static program the
   archive member that registers that resource is not linked in (nothing references it), so the resource did not
   exist, the template list was empty, and the menu held one separator, which Qt does not show as a submenu.
   `tests/knewmenu` (offscreen, a few seconds) printed "resource roots: kf6 org.kde.kcolorscheme qpdf
   qt-project.org" and one separator. Fix (native/patches/nativeqt-kio.diff): `Q_INIT_RESOURCE(templates)` in
   `KNewFileMenuPrivate::slotFillTemplates()`. Now: Folder..., Text File..., Empty File..., HTML File..., Link to
   Location (URL)..., Link to File or Directory..., Link to Application... (shot r5e/menu.png).
2. **The widget explorer's tiles had names but no pictures.** The picture of a tile (the widget's screenshot, or its
   icon) is drawn inside `iconWidget`, which Plasma's AppletDelegate.qml shades with a
   `KGraphicalEffects.BadgeEffect` (org.kde.graphicaleffects, kdeclarative: a ShaderEffect) fed by a
   `ShaderEffectSource { sourceItem: iconWidget; hideSource: true }`. Qt Quick's software renderer (what plasmashell
   runs with here) draws no ShaderEffect, and `hideSource` had hidden the picture: the same class as Klipper's
   OpacityMask. Fix (native/patches/nativeqt-kdeclarative.diff): BadgeEffect.qml sets `source.hideSource = false` when
   `GraphicsInfo.api === GraphicsInfo.Software` (the cut-out for the "New!" or running badge is not made; the badge
   is drawn over the picture). Now: previews (Activity Bar's tabs, the pager's boxes, Analog Clock, Application Menu,
   Calendar, Calculator, Color Picker, ...) in the explorer (shot r5x/e1.png).
3. **Binary Clock showed a blank page icon.** Not reproduced: its compact representation (a grid of dots drawn
   with Rectangles, no icon in it) is drawn at the default size (148x52) and at 276x196, in every run of the sweep
   below, and its popup (the month) opens. What showed a *blank page* in the sweep was **Fifteen Puzzle**: its icon
   `fifteenpuzzle` is kdeplasma-addons' own (hicolor/scalable/apps/fifteenpuzzle.svgz, not in Breeze), and the share
   bundle carried only share/plasma. The bundle now carries share/icons too:
   the puzzle's green tile is shown. (If the blank page seen was the Binary Clock after all: the dots need about
   46 px of height, and in a thinner panel nothing is drawn; that is the widget's own sizing.) Every other icon the
   widgets' metadata names was checked against the disk's icons (3800 names): all there.
4. **Memory Usage (and all system monitor widgets) showed no data.** Three things, in this order:
   - *No ksystemstats.* The widgets' sensors come from the user D-Bus service `org.kde.ksystemstats1`; neither the
     page's Debian disk nor the native build has it. Cheapest route, checked: **Debian trixie's `ksystemstats`
     (6.3.4, riscv64) from apt, run under rv** (3 small packages with libnl, 35 s under Node; D-Bus activates it
     with its own .service file the first time a sensor is asked for; no change to `desktop` needed). Memory,
     CPU (total and per core), network and disk activity show numbers; the native alternative (ksystemstats 6.3.6
     with its plugins linked in, libnl and libsensors built) was fetched and looked at, not built: the plugins
     are loaded as MODULEs (static registration again) and libnl is new, for a daemon that costs one apt install.
   - *`Plasmoid.faceController` read before it exists.* With the daemon there, the widgets stayed blank. The C++
     applet makes its face controller in `init()` and declares the property CONSTANT; for a widget added to a running
     desktop libplasma makes the QML first (ContainmentItem::appletAddedForward on appletAboutToBeAdded, before
     Containment::addApplet calls `init()`), FullRepresentation.qml's `Plasmoid.faceController.fullRepresentation`
     (no `?.`) read null, threw, and a CONSTANT property never tells anyone: blank for good. (Applets restored from a saved
     layout are probably made before the UI, which would hide it: not tested.) `plasma-shell-only.py` declares it with NOTIFY and
     `init()` emits `faceControllerChanged()`: the binding runs again. One `TypeError ... 'fullRepresentation' of
     null` per widget stays in the log; harmless.
   - *Disk Usage* shows 0 B / 0%: Solid finds no volumes without udev's database (the disks plugin lists them
     through Solid); Hard Disk Activity and Network Speed show 0 B/s because nothing is going on.
   The install step: `debian "apt-get install -y ksystemstats"` once (the network of the page's machine:
   net-setup.sh and net-relay.py running in the VM), then restart plasmashell.
5. **Widget sweep.** All 58 widgets the packages offer, added one by one to the desktop (default layout, 1920x1080;
   plasmanative's build/nm/extra/nq/r5s.sh, widgets.txt; the log lines of each load, then a screenshot with each
   widget's geometry read back over D-Bus), then 15 compact ones clicked (r5p.sh; shots r5p2/c*.png). Table below.
   Not in it: the store widgets (Bigclock, Bix Player, Start Next Menu, ...) and the containments
   (org.kde.desktopcontainment, org.kde.panel); `org.kde.plasma.private.systemtray` is the tray's own containment,
   not a widget (the explorer does not list it): adding it as a widget aborts plasmashell in
   `PlasmoidModel::PlasmoidModel` ("table index is out of bounds"), as forcing a containment into an applet
   slot would anywhere.

Result: **52 work, 5 partly, 1 fails.**

| widget (plugin id) | status | what is seen, and the cause |
|---|---|---|
| Search (org.kde.milou) | works | compact icon, popup (round 4) |
| Activity Bar (activitybar) | works | tab "Default" |
| Activity Pager (activitypager) | works | |
| Analog Clock (analogclock) | works | popup: the month |
| Global Menu (appmenu) | works | its placeholder text; no application exports a menu |
| Calendar (calendar) | works | |
| Camera Indicator (cameraindicator) | partly | "unavailable" icon: no PipeWire. Failed to load before (module org.kde.pipewire.monitor): stand-in QML module in plasmashell (qml-stubs/pipewire-monitor) |
| Clipboard (clipboard) | works | Klipper; popup "Clipboard is empty" |
| Disks & Devices (devicenotifier) | works | "No removable devices attached" (no udev: nothing is ever attached) |
| Digital Clock (digitalclock) | works | popup: the month |
| Folder View (folder) | works | as a widget: the (empty) folder's title |
| Icon (icon) | works | the default, no target: a generic page icon is what it shows |
| Icons-Only Task Manager (icontasks) | works | |
| Keyboard Layout (keyboardlayout) | works | hides itself with one layout; failed to load before (module org.kde.plasma.private.kcm_keyboard): stand-in (qml-stubs/kcm_keyboard: no flag icons, the country code as text) |
| Application Menu (kicker) | works | cascading menu opens |
| Application Launcher (kickoff) | works | popup opens (round 3) |
| Input Method Panel (kimpanel) | works | blank until an input method framework runs |
| Lock/Logout (lock_logout) | partly | buttons drawn; no session backend ("Could not load a session backend"), so lock and logout do nothing |
| Input Method (manage-inputmethod) | works | keyboard icon |
| Margins Separator (marginsseparator) | works | blank by design |
| Media Player (mediacontroller) | works | "No media playing" (an MPRIS player on the bus would show) |
| Minimize All Windows (minimizeall) | works | |
| Notifications (notifications) | works | "No unread notifications"; logs the missing org.kde.plasma.private.volume (plasma-pa, not built) once |
| Pager (pager) | works | |
| Panel Spacer (panelspacer) | works | blank by design |
| Show Activity Manager (showActivityManager) | works | side panel opens |
| Peek at Desktop (showdesktop) | works | |
| System Monitor Sensor (systemmonitor) | works | "Configure..." until sensors are chosen |
| Total CPU Use, Individual Core Usage, Hard Disk Activity, Memory Usage, Network Speed (systemmonitor.cpu, .cpucore, .diskactivity, .memory, .net) | works | numbers with ksystemstats installed (item 4); blank charts without it |
| Disk Usage (systemmonitor.diskusage) | partly | 0% / 0 B: Solid has no volumes without udev |
| System Tray (systemtray) | works | the up-arrow popup |
| Icons-and-Text Task Manager (taskmanager) | works | |
| Trashcan (trash) | works | |
| Window List (windowlist) | works | nothing to list; no popup without windows |
| Display Configuration (kscreen) | works | "Screen Layout", presentation mode; one display, so the layouts are disabled |
| Kate Sessions (addons.katesessions) | works | "Start Kate", "New Kate Session" |
| Binary Clock (binaryclock) | works | see item 3 |
| Calculator (calculator) | works | |
| Color Picker (colorpicker) | works | |
| Comic Strip (comic) | partly | asks to be configured; strips come from the network, and Qt has no QNetworkInformation backend here ("connect(QNetworkInformation ...): invalid nullptr"); the QML's first reads of `comicData` fail (the same order as item 4) |
| Disk Quota (diskquota) | partly | "Quota tool not found, please install 'quota'": by design (apt install quota) |
| Fifteen Puzzle (fifteenpuzzle) | works | the icon is shipped now (item 3); its popup plays; the board's invisible Image logs "Failed to get image from provider" (the widget's own) |
| Fuzzy Clock (fuzzyclock) | works | "Five past three" |
| Grouping Plasmoid (grouping, private.grouping) | works | the empty container "Drag applets here"; QML warnings of its own |
| Lock Keys Status (keyboardindicator) | works | |
| Application Dashboard (kickerdash) | works | full-screen launcher opens |
| Konsole Profiles (konsoleprofiles) | works | |
| Media Frame (mediaframe) | works | "No items available": no folder of pictures set |
| Sticky Note (notes) | works | |
| Quicklaunch (quicklaunch) | works | |
| Timer (timer) | works | 12 x "connect(QObject, Plasma::Applet): invalid nullptr" and a `TimerView.qml` width error at start: the widget's own (it splices its predefined timers into contextualActions) |
| User Switcher (userswitcher) | works | "root" |
| Weather Report (weather) | fails | "Configure...", and "Could not find plugin": the weather data engine and its ions (plasma-workspace dataengines/weather) are not built (shell-only build of plasma-workspace); the widget needs the network anyway |

Left, with causes: Weather (the engine and five ions would have to be built and linked statically, and registered
like the other engines; then the network of the page's machine), Comic Strip (QNetworkInformation's Linux backends
need NetworkManager), Lock/Logout (a session backend: nothing in the page provides logout), Disk Usage (udev),
Camera Indicator (PipeWire), the QML warnings of Timer, Grouping and Comic.

The class of finding in 1 and 2 -- static linking and the software renderer -- has a third relative to remember:
a static archive drops an object file nothing references, with the resource registration or static plugin in it.

### What plasmashell asks of the window server (for the wlserver work)

wlserver offers: wl_compositor, wl_subcompositor, wl_shm, wl_seat, wl_output, wl_data_device_manager,
xdg_wm_base, zxdg_decoration_manager_v1, org_kde_kwin_server_decoration_manager, xdg_activation_v1,
zxdg_output_manager_v1, wp_viewporter, wp_cursor_shape_manager_v1, zwp_text_input_manager_v3,
zwp_primary_selection_device_manager_v1, zwp_linux_dmabuf_v1. Both plasmashells (native and riscv64)
bound exactly those. What Plasma 6.3's shell looks for besides, and what it does without each:

| protocol | used for | without it (seen or in the code) |
|---|---|---|
| zwlr_layer_shell_v1 (LayerShellQt) | the desktop and the panels as layers (background, top), anchored to edges, exclusive zones | "Failed to initialize layer-shell integration": the desktop is an ordinary xdg toplevel (decorated, movable, raised over other windows); panels the same, reserving no space |
| org_kde_plasma_shell (KWayland PlasmaShell) | panel role and position, panel behaviour (always visible / auto-hide / windows go below), skip taskbar/switcher for its own windows, on-screen displays | panels and popups placed by the compositor, not where Plasma puts them; Plasma's own surfaces show in task lists |
| org_kde_plasma_window_management | the task manager's list of windows, activating/minimizing/closing them, geometry for animations | "PlasmaWindowManagement protocol hasn't activated in time": task manager empty |
| org_kde_plasma_virtual_desktop_management | the pager, virtual desktops in the task manager | pager shows nothing / one desktop |
| kde_output_order_v1 | which screen is primary and their order | "OutputOrderWatcher may not work": Qt's order of screens used (fine with one) |
| kde_screen_edge_v1 | auto-hiding panels shown at the screen edge | auto-hide panels cannot be brought back |
| org_kde_kwin_blur, org_kde_kwin_contrast, org_kde_kwin_slide, org_kde_kwin_shadow_manager | the panel's and popups' translucent blur, contrast, sliding in, shadows | flat, unblurred, unshadowed |
| org_kde_plasma_activation_feedback | launch feedback in the task manager | no busy indication while a program starts |
| org_kde_kwin_appmenu_manager | the global menu applet | global menu empty |

(The page's own desktop, `desktop wayland`, draws its panel itself and keeps plasmashell's role to the
wallpaper over D-Bus: wlserver/PLASMA-PROTOCOL.md.)

### Build times (wall clock, VM, -j10 until 2026-10-10 14:20, then -j6)

| Step | Time |
|---|---|
| libc++/libc++abi (build-runtimes.sh) | 9 s |
| libwayland + libxkbcommon | under a minute |
| qtbase (static, all modules but SQL) | 114 s; incremental after a flag change ~60-85 s with ccache |
| qtwayland | 43-59 s |
| Mesa (softpipe + virgl, no LLVM) + libdrm | 65 s |
| qtshadertools | 39 s |

## Dependencies built so far

- libwayland 1.23.1 (client, cursor, server) without libffi:
  `native/patches/nativeqt-wayland.diff` + `native/wayland-noffi.py`.
  `wl_closure_invoke` calls a listener through a switch over every
  signature of up to 10 arguments, each 32-bit or a pointer (2047 cases);
  libffi has no wasm64 port and Wasm cannot call a signature chosen at run
  time.
- libxkbcommon 1.7.0 (no X11, no registry, no tools).
