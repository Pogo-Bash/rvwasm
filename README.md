# rvwasm

**A Linux desktop that runs inside a browser tab.** rvwasm boots Debian (riscv64) with KDE Plasma in Chrome:
Konsole, Dolphin, Kate, System Settings, Firefox, Chromium, `apt-get install`, sound, GPU drawing and hardware
video decoding. Nothing is streamed from a server: the machine runs on the visitor's own computer, inside the
page. The server only hands out the files and, optionally, a restricted network relay.

## Architecture

```
 Browser tab (the hardware)
 ├─ display ........ draws the windows (WebGL)
 ├─ GPU executor ... runs the machine's OpenGL on WebGL2 / WebGPU
 ├─ video decoder .. WebCodecs (the computer's hardware decoder)
 ├─ audio player ... AudioWorklet
 └─ network ........ WebSocket to the relay
        │  one block of shared WebAssembly memory
 Linux kernel compiled to WebAssembly (linux-wasm; no MMU, every task a Web Worker)
        │
 Machine services, in JavaScript: wlserver (Wayland compositor), gpuserver, plus PulseAudio
        │
 Programs
 ├─ Debian riscv64 programs ──► rv, our riscv64 → WebAssembly JIT
 └─ native WebAssembly programs: the KDE desktop shell (plasmashell, Qt 6, KDE Frameworks)
```

- **Kernel:** Joel Severin's [linux-wasm](https://github.com/joelseverin/linux-wasm) with our patches (memory
  sizing, signal and trap fixes, a sound card). Processes are Web Workers sharing one memory; there is no
  `fork()` (programs use `vfork`/`posix_spawn`).
- **rv:** runs Debian's unmodified riscv64 binaries by translating hot code to WebAssembly, which the browser
  compiles to machine code. Every instruction is translated twice, so this is where speed goes.
- **Native desktop:** the KDE shell is compiled straight to WebAssembly (no emulation), so the desktop itself
  stays responsive.
- **Disks:** the system disk is fetched in 1 MiB pieces as the machine reads them and kept in the browser
  (Origin Private File System); writes and the home folder stay in the visitor's browser.
- **GPU, video, sound:** programs' OpenGL commands, compressed video and audio go through the shared memory to
  the page, which runs them on the real GPU, the hardware video decoder and the sound output.

More detail on every part: [docs/overview.md](docs/overview.md).

## Setup with Docker

Requirements on the server: Linux with Docker and Compose v2, and `/dev/net/tun` for the relay.

```
git clone <this repository> rvwasm && cd rvwasm
# put the built release in ./dist (docker/release.sh builds it; or set RV_RELEASE_URL to a release archive)
docker compose up -d
```

- **Local use:** open **http://localhost:8080/** for the landing page and click Start Plasma, or go straight to
  **http://localhost:8080/desktop**, which boots at once (localhost counts as a secure context, so no certificate is
  needed).
- **Public use:** the page needs HTTPS (shared memory and cross-origin isolation only work in a secure
  context). The deployment puts Caddy in front with an automatic Let's Encrypt certificate; set `RV_DOMAIN` to
  your domain and point the domain's A record at the server (`docker compose --profile public up -d`). The
  project's own public instance runs at **https://plasmaontheweb.com/**. Details, including switching domains and
  updating the files: [docs/deploy.md](docs/deploy.md).
- Ports: the page on 8080 and the relay on 8081 behind Caddy (443). Open 80 and 443 in the server's firewall
  (and, on Oracle Cloud, in the instance's security list).

Click the page once after it loads (browsers only allow sound after a click). The desktop starts by itself;
`?autostart=0` boots to a shell prompt instead (`/desktop?autostart=0`, or `/?autostart=0` through the landing page's
Start button). The landing page (`site/`: `index.html`, `robots.txt`, `sitemap.xml`, icons, `assets/`) is what search
engines see; `/desktop` is the machine and is not indexed. Other switches: `?mem=MiB`, `?audio=0`, `?vdec=off`,
`?exec=webgpu`, `?net=off`, `?fresh`.

## Network and DNS limitations

The machine reaches the internet only through the relay, and the public relay is **restricted on purpose**, so
a public demo cannot be used for spam, scanning or anything else under the operator's name:

- **DNS answers only for allowlisted domains.** The relay's own DNS server resolves those names and nothing
  else; other DNS servers are blocked.
- **Connections only go to addresses those names resolved to, on ports 80 and 443.** Hard-coded IP addresses,
  other ports (mail, SSH, games) and private networks are dropped.
- **Per-visitor limits** on connections and rate.
- **The default allowlist** covers installing software: Debian's archive (`deb.debian.org`,
  `security.debian.org`), Python (`pypi.org`, `files.pythonhosted.org`), npm (`registry.npmjs.org`), GitHub
  (`github.com` and its download hosts) and Rust (`crates.io`). It is one text file the operator can edit.

What that means for visitors: `apt-get install`, `pip install`, `npm install` and `git clone` from GitHub work;
general web browsing from inside the machine does not, unless the operator adds the sites. A self-hosted
relay can be opened up further; whoever runs a relay is responsible for the traffic that leaves through it.

## Recommended specs (for visitors)

rvwasm runs on the visitor's computer, so their machine is what matters, and **per-core speed matters most**:
the riscv64 JIT, the kernel's Workers and the desktop shell are mostly single-threaded per task, so a fast core
helps far more than many slow ones.

| | minimum | recommended |
|---|---|---|
| CPU | 4 cores, recent | **fast single-core performance** (Apple M1 or newer, recent Intel Core / AMD Ryzen); 8+ cores |
| RAM | 8 GB (a light session) | **16 GB** (the tab uses 4-8 GB with the desktop) |
| Browser | current desktop Chrome / Edge / Brave | same; Firefox, Safari and phones are not supported |
| Disk | 5 GB free for the browser's copy of the disk | |
| GPU | any WebGL2 GPU | a GPU with WebGPU for `?exec=webgpu` |

Low-power laptops and Chromebooks boot it, but programs under emulation (Firefox, Chromium, compiles) will be
slow there.

**For the server:** it only serves static files and the relay: 2 cores, 4 GB RAM and about 15 GB of disk are
enough. Bandwidth is the real cost: each new visitor downloads the parts of the disk they use (hundreds of MB
to a few GB). Serve the disk pieces directly, not through a CDN that limits large files.

## Credits

linux-wasm (Joel Severin), Debian, KDE and Qt, Mesa and virglrenderer, PulseAudio, mpv and libva, BusyBox,
musl, LLVM.

## Licence

rvwasm's own code is under the **rvwasm Noncommercial Share-Alike License 1.0** (`LICENSE.md`): free to use,
change and share for any noncommercial purpose, and anything you build on it and share with others (including
over a network) must be published as complete source, for free, under the same licence. It is not for ads,
resale or running as a business. It is based on the PolyForm Noncommercial License 1.0.0, modified, and is not
endorsed by the PolyForm Project.

The project also ships and builds on other people's software (Linux through linux-wasm, musl, BusyBox, Debian,
Qt, KDE, Mesa, virglrenderer, PulseAudio, mpv and more), which keeps its own licences, mostly GPL, LGPL and MIT.
`LICENSE.md` says which paths of this repository are under which licence; `THIRD-PARTY.md` lists the components
of the built release, their licences and where their source is (the native desktop shell's build scripts and
patches are in `native/`). `TRADEMARKS.md` covers the names rvwasm and plasmaontheweb.com, and says that this is
an unofficial project, not affiliated with KDE.

Questions and source requests: open an issue at <https://github.com/Pogo-Bash/rvwasm>.
