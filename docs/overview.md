# rvwasm

**A real Linux desktop in a browser tab.** rvwasm runs Debian (riscv64) with KDE Plasma inside a single
Chrome tab: Konsole, Dolphin, Kate, System Settings, Firefox, Chromium, `apt-get install`, sound, the GPU,
hardware video decoding. Nothing is streamed from a server: the machine runs on your own computer, inside
the page.

```
docker compose up
```

then open **http://localhost:8080/** in desktop Chrome. The machine boots and the KDE desktop starts by itself.

---

## Contents

1. [How it fits together](#how-it-fits-together)
2. [The kernel: linux-wasm](#the-kernel-linux-wasm)
3. [rv: the riscv64 emulator and JIT](#rv-the-riscv64-emulator-and-jit)
4. [The Debian system and its disks](#the-debian-system-and-its-disks)
5. [The page](#the-page)
6. [wlserver: the window server](#wlserver-the-window-server)
7. [The GPU](#the-gpu)
8. [Hardware video decoding](#hardware-video-decoding)
9. [Sound](#sound)
10. [Native Qt and KDE (plasmashell in WebAssembly)](#native-qt-and-kde)
11. [hostnode: JavaScript programs on the browser's own engine](#hostnode)
12. [The network](#the-network)
13. [Talking to the host](#talking-to-the-host)
14. [Running it: the release and Docker](#running-it)
15. [Switches](#switches)
16. [Building from source](#building-from-source)
17. [Testing](#testing)
18. [Limits and known issues](#limits-and-known-issues)
19. [Repository layout](#repository-layout)
20. [Credits](#credits)

---

## How it fits together

The browser tab is the hardware. A Linux kernel compiled to WebAssembly runs on it. Programs run on that kernel
in two ways: Debian's ordinary riscv64 programs through `rv`, our riscv64-to-WebAssembly JIT, or programs
compiled straight to WebAssembly. Our own services, written in JavaScript and run as processes of the machine,
connect the programs to the page: windows, the GPU, sound, video.

```
 Browser tab (the hardware)
 ├─ Display ............ composites the windows (display.js)
 ├─ GPU executor ....... runs GPU commands on WebGL2 or WebGPU
 ├─ Video decoder ...... WebCodecs, the computer's hardware decoder
 ├─ Audio player ....... an AudioWorklet
 └─ Network ............ a WebSocket to rvRelay
        │  one shared block of memory between the page and the machine
 linux-wasm kernel (Linux compiled to wasm64, no MMU, every task a Web Worker)
 ├─ devices: keyboard, pointer, framebuffer, sound card, disks, network
        │
 Machine services (ours, JavaScript run as processes)
 ├─ wlserver ........... the Wayland compositor
 ├─ gpuserver .......... carries GPU commands to the page
 └─ PulseAudio ......... Debian's sound server, on our sound card
        │
 Programs
 ├─ Debian riscv64 programs  ──►  rv (riscv64 → WebAssembly JIT)
 └─ native WebAssembly programs (plasmashell, Qt, KDE Frameworks)
```

Everything shares one block of WebAssembly memory. That is how the page reads a window's pixels, a GPU
command ring or a sound buffer straight out of the machine without copying it through messages.

---

## The kernel: linux-wasm

The kernel is Joel Severin's [linux-wasm](https://github.com/joelseverin/linux-wasm): Linux compiled to
64-bit WebAssembly (`wasm64`), with a JavaScript runtime that starts it in the page.

How it differs from Linux on a normal computer:

- **No MMU.** It is a NOMMU kernel: there is one address space, and every process lives in the same memory.
  There is no copy-on-write `fork()`: `vfork()`, `posix_spawn()` and `clone()` work, plain `fork()` does not.
- **Every task is a Web Worker.** Each process and thread runs in its own Worker; they all share the one
  `memory64` WebAssembly memory. The kernel schedules them and they block with `Atomics.wait`.
- **System calls** are calls from the program's WebAssembly into the kernel's.

Our changes, kept as patch sets in `patches/`:

- **memsize:** the machine's memory is sized from the computer (4–16 GiB) instead of fixed at 3 GiB.
- **The CPU pool** (`wasm_pool=`): how many Workers run programs at once.
- **lwfix:** fixes from our audit of linux-wasm: a trap with a handler that hung forever, SIGKILL never reaching
  a spinning task, `SA_RESTART` running the handler late, `fork()` now failing cleanly with `ENOSYS`, 64-bit
  atomic compare-and-swap, the signal frame layout.
- **sigfix:** thread stacks aligned to 16 bytes, the `SA_SIGINFO` pointer read from the frame, `syscall()`'s
  integer arguments on wasm64. Reported upstream as linux-wasm issues #19–#22.
- **The sound card** "Wasm audio" (below).

The runtime (the JavaScript that boots the kernel and runs its Workers) is patched by `tools/patch-runtime.py`,
a numbered list of changes: input devices, the framebuffer, the disks, the GPU and audio hand-offs, crash
reporting, and more.

---

## rv: the riscv64 emulator and JIT

linux-wasm can only run programs compiled to WebAssembly, and almost no Linux software is built that way.
So rvwasm runs **Debian's own riscv64 build** (the same packages a RISC-V board installs) through `rv`, a
riscv64 user-mode emulator that is itself a WebAssembly program on the kernel.

RISC-V was chosen because its instructions are simple and regular, cheap to translate, and Debian supports it
fully.

What rv does:

- **Loads riscv64 ELF programs** and their shared libraries (it has its own dynamic linker support).
- **Interprets cold code, translates hot code.** Code that runs often is translated, a region at a time, into a
  WebAssembly module, which the browser then compiles to machine code. Translations are batched (32 regions per
  module) and shared between threads and processes: the browser caches compiled modules by their bytes.
- **Passes system calls through** to the kernel, translating riscv64's numbering and structures: files,
  sockets, threads (`clone`), signals, `mmap`, `ioctl` (including ALSA's and DRM's), `seccomp`, `chroot`.
- **Emulates the GPU render node** `/dev/dri/renderD128` (rv-virtgpu), so Mesa's virgl driver talks to it
  like real hardware.
- **Keeps translated code under a budget** (1536 MiB by default, `?jitcode=`): the browser caps compiled
  WebAssembly at about 4 GiB per tab, and very large programs (Chromium) would otherwise exhaust it.

Supported: RV64GC (integer, multiply, atomics, single and double float, compressed instructions) and the bit
manipulation extensions. Not yet: the vector extension (see `docs/backlog.md`).

Speed, measured against qemu compiled to WebAssembly: 8–18× faster on CPU work, 5.5–6.4× on process
creation. Still, every instruction is translated twice (riscv64 → wasm → machine code), so heavy programs run
slower than natively.

Debugging switches (environment variables in the machine): `RV_STRACE=1` (every system call), `RV_VERBOSE=1`,
`RV_JIT_HOT=N` (how hot before translating), `RV_JIT_CHECK=1` (check the JIT against the interpreter),
`RV_CRASH_MAPS=1` (print the memory map on a crash).

---

## The Debian system and its disks

The machine boots a small BusyBox system from its boot image, then enters **Debian trixie (riscv64)** from the
system disk (`debian` on the console).

- **The system disk** is an ext4 image cut into 1 MiB pieces. The page fetches pieces as the system reads them
  and keeps them in the browser's Origin Private File System, so a second visit starts from what is already
  stored. Writes stay in your browser.
- **The home disk** (`/root`) is a separate disk that exists only in your browser.
- **The release disk** already has everything the desktop needs: KDE Plasma, Konsole, Dolphin, Kate,
  System Settings, Firefox ESR, Chromium, PulseAudio, ALSA's tools, ksystemstats, mpv, the video driver, and
  the native plasmashell in `/opt/nq`.
- `apt-get install` works (through rvRelay), so anything else in Debian's riscv64 archive can be added.

Logs inside the machine: `/root/autostart.log` (the desktop), `/root/wlserver.log` (the window server),
`/root/sound.log` (the sound server).

---

## The page

`tools/www/` is the web page: `dev.html` and the scripts it loads.

- **display.js** draws the screen: it reads the window server's surface table from the shared memory, uploads
  what changed, and composites the windows on a canvas (WebGL). Window frames, title bars and their buttons are
  drawn by the page in the DOM, in KDE's look, read from the machine's Plasma settings (Breeze, Breeze Dark,
  Aurorae themes).
- **Input:** keys, the pointer, the wheel and touch go to the kernel's input devices. "Cmd is Ctrl" maps the
  Mac's Command key to Control. Text from the computer's input method (Japanese, Chinese, Korean) is passed
  through.
- **The clipboard** is bridged both ways between the machine and the computer.
- **Files to machine / Page to host:** drop files into the machine's home, or open a link in a real tab.
- **Background tabs keep running:** the page plays an inaudible tone so Chrome treats it as active, and the
  window server keeps its clock at 20 frames a second while the tab is hidden.
- **Each tab is its own machine:** the page's channels to the machine are named per tab, so an old tab cannot
  take over a new one.

---

## wlserver: the window server

`wlserver/` is a **Wayland compositor written in JavaScript**, run as a process of the machine on the
browser's own JavaScript engine (see hostnode). Programs connect to it as to any Wayland compositor; the page
draws their windows from the shared memory.

What it implements:

- **Core Wayland and xdg-shell:** windows, popups, positioning, resizing, maximising, minimising,
  server-side decorations.
- **Layer shell** (`zwlr_layer_shell_v1`): plasmashell's desktop and panels.
- **Plasma's protocols:** plasma shell surfaces, window management for the task manager and pager, virtual
  desktops, output order.
- **Input:** keyboard with 30 compiled XKB layouts, pointer, focus, text input (`zwp_text_input_v3`).
- **The clipboard and drag and drop**, bridged to the computer's clipboard.
- **dmabuf** (`linux_dmabuf_v1`): GPU-rendered windows presented without copying.
- **X11 programs** through Xwayland, started on demand, with an X window manager and the clipboard between X
  and Wayland.

`desktop wayland PROGRAM` (`tools/x/desktop`) starts the server if needed, a session bus, the sound server and
the program. `desktop wayland plasma-native` starts the full desktop.

---

## The GPU

Programs in the machine draw with OpenGL (and Vulkan) through Mesa, and the commands run on **your computer's
real GPU** through the browser.

```
program → Mesa virgl driver → rv's render node / vtest socket
       → gpuserver (in the machine) → shared command ring
       → the page's GPU executor → WebGL2 (or WebGPU) → your GPU
```

- **gpuserver** (`gpu/server/`) receives virgl's command stream and puts it on a ring in the shared memory.
- **The executor** runs in the page: **vrend** (virglrenderer's renderer, compiled to WebAssembly) on WebGL2,
  or with `?exec=webgpu`, vfront → zink → **wgvk**, our Vulkan-on-WebGPU layer.
- **Vulkan** for programs goes through venus on the WebGPU executor (early; not in every release).
- **Zero-copy present:** GPU-rendered windows are handed to the window server as dmabufs, and the page
  draws them directly.
- `RVGPU=0` (or `?gpu=off`) falls back to software rendering (llvmpipe).

---

## Hardware video decoding

Decoding video in software through the emulator is far too slow for HD. So players in the machine hand the
**compressed** video to the page, which decodes it with **WebCodecs**: your computer's hardware decoder.

- `rvwasm_drv_video.so` is a **VA-API driver**: the standard Linux interface mpv, VLC, GStreamer, Firefox and
  Chromium already use for hardware decoding. Formats: H.264, HEVC (where the browser supports it), VP8, VP9,
  AV1.
- Decoded frames stay in the page as GPU textures and are drawn into the player's window without being copied
  back into the machine.
- The release's `mpv` uses it by default: `mpv FILE` in Konsole. `rv-video-check` prints a diagnostic report.
- `?vdec=off` makes players decode in software.

---

## Sound

- The kernel has a sound card, **"Wasm audio"**: a PCM playback device with four streams, volume and mute.
  Its buffers live in the shared memory.
- The page plays them with an **AudioWorklet**, which mixes the streams, resamples to the browser's rate and
  tells the kernel how far it has played, so programs keep real time.
- **PulseAudio** runs on the card (PipeWire works too), so Firefox, Chromium, mpv and the desktop's sounds all
  play.
- Chrome only allows sound after you click the page once. `?audio=0` turns sound off.
- No microphone yet.

---

## Native Qt and KDE

The desktop shell runs **natively in WebAssembly**, not through the emulator, so it is fast:

- **nq-clang**, our toolchain: clang 18 for `wasm64` with libc++, WebAssembly exceptions and setjmp/longjmp,
  linking each program as a static position-independent executable.
- **Qt 6.8 and the KDE Frameworks 6 and Plasma 6.3 packages it needs** (about 100), built from source with it.
  Plugins are linked in statically and found by name.
- **plasmashell** (the desktop, the panel, Kickoff, the system tray, the widgets) is one 134 MiB WebAssembly
  program. It uses Debian's QML files, themes and icons from the disk, plus a bundle of the add-on widgets
  Debian's disk lacks.

Because everything is linked in, a widget or theme that needs a compiled module we did not build will not load;
most of KDE's own widgets work (52 of 58 in our sweep). The fix for good is dynamic libraries for native
WebAssembly programs, prototyped on branch `wasmdyn`.

Other programs (Konsole, Dolphin, System Settings) are Debian's riscv64 builds through rv. Both kinds share
one desktop.

---

## hostnode

`hostnode/` runs JavaScript programs as processes of the machine on the **browser's own JavaScript engine**
(V8), at full speed, with the machine's system calls available to them. wlserver and gpuserver are hostnode
programs. It is a trusted tool for our own services: such a program sees the whole machine's memory, so it is
not a way to run untrusted JavaScript.

---

## The network

The machine has real networking. Its network card sends Ethernet frames over a WebSocket to **rvRelay**, which
puts them on a TAP device and translates addresses out to the internet.

- `tools/net-relay.py` is rvRelay. It only accepts the page's own origin.
- In the release, rvRelay runs in its own container (`NET_ADMIN` and `/dev/net/tun` in that container only),
  on 127.0.0.1.
- The rvRelay you run is yours: you are responsible for the traffic that leaves your machine through it.
- `?net=off` turns networking off.

---

## Talking to the host

- **hostopen:** `rv-host-open URL` (or a link opened from a program) asks the page to open it in a real browser
  tab.

---

## Running it

### Requirements

- **Browser:** desktop Chrome or another Chromium (Edge, Brave), current. It needs shared memory (the server
  sends the cross-origin isolation headers), 64-bit WebAssembly memory, the Origin Private File System and
  WebCodecs. Firefox, Safari, phones and tablets are not supported.
- **Computer:** about **16 GB of RAM** (the tab takes 4–8 GB with the desktop) and 8 or more cores. About 5 GB
  free for the browser's copy of the disk.
- **Docker host:** Linux with Docker, Compose v2 and `/dev/net/tun`. On a Mac or Windows, a Linux VM that
  forwards its ports works (the project uses Lima).

### Start

```
docker compose up
```

Open **http://localhost:8080/**, click the page once (for sound). The desktop comes up by itself. The
terminal under the screen is the machine's console and is usable at once.

Ports: the page on `RV_HTTP_PORT` (8080), rvRelay on `RV_NET_PORT` (8081), both on 127.0.0.1 only. The page
finds rvRelay itself. `docker compose down` stops both.

`docker compose up` serves what is in `./dist`. Build it with `docker/release.sh`, or set `RV_RELEASE_URL` to a
release archive and the first `up` downloads it.

---

## Switches

Added to the address, e.g. `http://localhost:8080/?autostart=0&mem=6144`:

| switch | what |
|---|---|
| `?autostart=0` | boot to the shell prompt; start the desktop by hand with `debian /etc/rvwasm/autostart &` |
| `?mem=MiB` | the machine's memory (default: from what the browser reports, 4–16 GiB) |
| `?exec=webgpu` | GPU commands through the WebGPU executor instead of WebGL2 |
| `?gpu=off` | software rendering only |
| `?audio=0` | no sound |
| `?vdec=off` | software video decoding |
| `?jitcode=MiB` | rv's translated-code budget |
| `?fresh` | forget what was written to the system disk in this browser (`?fresh-home`: the home disk) |
| `?net=off` | no network |

---

## Building from source

On a Linux build machine with Docker (the project's is a Lima VM named `rvwasm`, where `~/rvwasm` holds the
Debian KDE root and the native Qt build):

```
docker/release.sh            # bundle root disk code dist up
```

| step | what |
|---|---|
| `bundle` | the native plasmashell and its QML packages (`tools/nq-bundle.sh`) |
| `root` | the release root: the KDE root plus PulseAudio, ALSA's tools, ksystemstats, mpv, the VA-API driver, the native plasmashell, the autostart (`tools/build-debian-image.sh release-root`) |
| `disk` | the 8 GiB system disk in 1 MiB pieces, and the empty home disk |
| `code` | in the build container: the kernel (with the sound card), musl, BusyBox, rv, vrend, the boot image, the page |
| `dist` | everything into `./dist`, plus `dist/RELEASE.txt` |
| `up` | `docker compose up -d --build` |

From a Mac, `tools/release-setup.sh` copies the tree into the VM and runs all of it there. Building everything
from nothing (LLVM, the Debian roots, Qt and KDE) is described in `docs/docker.md` and `docs/native-wasm.md`.

---

## Testing

- **Native suites:** `make -C tests check` (rv's guest tests) and `tools/*-check.sh` (BusyBox, Debian, threads,
  dynamic linking, seccomp, self-modifying code, chroot, sshd), run against the native build of rv.
- **node-machine** (`tools/node-machine.js`) boots the whole machine in Node, without a browser: the kernel, rv,
  the window server (with screenshots of its windows), the GPU and video through stand-ins for the page. Most
  work is verified there first.
- **Browser checks** are run by hand in Chrome (for video, `tools/x/rv-video-check` inside the desktop).

---

## Limits and known issues

- **No `fork()`**, so programs that need a real copy-on-write fork must use `vfork`/`posix_spawn` instead
  (most do through glibc).
- **No systemd.** The first process is our own startup script; services are started by the `desktop` script and
  by D-Bus activation. `systemctl` does not work, and Plasma's logout/lock buttons do nothing.
- **Memory fragmentation:** with no MMU, memory can fragment over a long session; very large allocations can
  then fail.
- **The browser's compiled-code limit** (about 4 GiB per tab): very heavy programs (Chromium on large sites)
  can reach it and crash the tab ("Aw, Snap!" error 5).
- **Speed:** programs under rv run slower than natively; the native desktop shell and the GPU, sound and video
  paths do not have that cost.
- **Video in the release:** hardware decoding works, but the decoded picture does not reach the window yet in
  the browser (being fixed).
- **Panel context menus** open at the top of the screen (being fixed).
- **Chrome only.**

---

## Repository layout

| path | what |
|---|---|
| `rv/` | the riscv64 emulator and JIT (C): `jit.c`, `interp.c`, `syscall.c`, `mm.c`, `signal.c`, `thread.c`, `virtgpu.c` |
| `wlserver/` | the Wayland compositor (JavaScript) |
| `gpu/` | gpuserver, vrend for the page, the WebGPU path, the video decoder (`gpu/vdec`) |
| `hostnode/` | running JavaScript programs as processes of the machine |
| `hostopen/` | opening links on the host |
| `tools/www/` | the web page |
| `tools/x/desktop` | starting the desktop and programs |
| `tools/patch-runtime.py` | our changes to linux-wasm's runtime |
| `tools/node-machine.js` | the machine in Node, for tests |
| `patches/` | our patches to the kernel, musl and the runtime |
| `docker/`, `docker-compose.yml` | the release build and the containers |
| `tests/` | guest tests |
| `docs/` | design notes, upstream reports (`docs/upstream/`), `docs/backlog.md` |

---

## Credits

rvwasm stands on a lot of other people's work:

- **linux-wasm** by Joel Severin: Linux compiled to WebAssembly, the foundation everything runs on.
- **Debian**: the riscv64 system and its packages.
- **KDE**: Plasma, the Frameworks, and the applications; **Qt**.
- **Mesa** and **virglrenderer**: the OpenGL and Vulkan paths.
- **PulseAudio**, **mpv**, **libva**, **BusyBox**, **musl**, **LLVM/clang**.

The licences of rvwasm and of everything it contains: `LICENSE.md`, `THIRD-PARTY.md`.
