# The machine with Docker

Two images and one compose file (`docker-compose.yml` at the top of the repository). The short version is
README.md, "Run it".

- **run** (`docker/Dockerfile.run`): the page's server (`docker/serve.py`: the COOP/COEP headers the page needs
  for shared memory, no caching, several requests at once, `/rv-site.json` with the relay's port and the default
  disk) and the network relay (`tools/net-relay.py`, with the TAP device and address translation of
  `tools/net-setup.sh` inside its own container). The image has the programs only: what they serve, the *dist*
  (`dist/www`: the page, the kernel, the boot image, the GPU executors; `dist/disk/NAME`: the disks), is mounted
  read-only from `${RV_DIST:-./dist}`. `docker/release.sh` or `tools/pack-www.sh` make it.
- **build** (`docker/Dockerfile.build`): everything it takes to build that dist from source: linux-wasm with
  its LLVM 18, Emscripten 6.0.11, the riscv64 cross compilers, qemu-user, debootstrap. `docker/build.sh` does
  the building with the scripts in `tools/`, into a volume (`/work`, laid out as `~/rvwasm` is in the build
  VM), from the source tree mounted at `/src`.

Docker has to run on Linux (here: in the Lima VM `rvwasm`, where `docker` needs `sudo`; Lima forwards the
ports to the Mac). Compose v2.

## Run

    docker compose up -d
    # open http://localhost:8080/

Services: `fetch` (one-shot: the dist is there, or it downloads `RV_RELEASE_URL`, a tar archive of one, into
it), then `www` and `relay`. `/` goes to `dev.html` with the query kept; without `?disk=` the page takes the
server's disk (`RV_DISK`, default `release`) and without `?net=` the relay's port the server names. Both ports
are published on 127.0.0.1 only. The relay takes WebSocket connections only from the page's origins
(`http://localhost:PORT`, `http://127.0.0.1:PORT`) and refuses others with 403.

| variable | default | what |
|---|---|---|
| `RV_DIST` | `./dist` | the dist (`docker/release.sh`, `tools/pack-www.sh`) |
| `RV_DISK` | `release` | the disk a page without `?disk=` gets (`RV_DISK=` for none) |
| `RV_HTTP_PORT` | 8080 | the page |
| `RV_NET_PORT` | 8081 | the relay |
| `RV_RELEASE_URL` | | a release archive to fetch when `RV_DIST` has none (`docker/release.sh tarball`) |
| `RV_ORIGINS` | | more origins the relay accepts, separated by spaces |
| `RV_BLENDER_PORT` | 8040 | Puter's Blender, when its profile is on |

Puter's Blender (`blenderweb/serve.py`) is off unless its profile is named:
`docker compose --profile blender up -d`. The first start downloads the release into the `blender` volume.

Stop: `docker compose down`. The services restart with Docker unless stopped.

### The release (`docker/release.sh`)

In the build VM, with what is built there (`~/rvwasm`: the KDE root after `dev kde` and `chromium kde`,
nativeqt's build in `~/rvwasm/nativeqt/sysroot`): `docker/release.sh` makes the native plasmashell's bundle, the
release root and its disk natively, runs the build container for the kernel (with the sound card), rv, vrend and
the page into `./dist`, links the disks in and starts the services. From the Mac: `tools/release-setup.sh`.
Its steps are in README.md and at the top of the script.

### From the build VM's own build (no build image needed)

What `tools/stage.sh` and `tools/build-debian-image.sh` made in `~/rvwasm/www` is packed and served:

    cd ~/dev/rvwasm        # in the VM; the commands below with sudo
    tools/pack-www.sh ~/rvwasm/www ~/rvwasm/dist
    RV_DIST=~/rvwasm/dist RV_DISK=daily docker compose up -d --build

`pack-www.sh` takes the page's files, the patched runtime, the kernel, the boot image, `vrend/` and `rvgpu/`
when they are there, and the disks named in `RV_DISKS` (default `daily home`; `RV_IMAGES="debian debian-x"`
adds those boot images). The disks are hard links when the dist is on the same file system as `www`, so the
dist costs nothing; keep it off the Mac's shared folders (they are another file system: 3.8 GB would be copied).
To try it next to other servers: `RV_HTTP_PORT=18000 RV_NET_PORT=18001 docker compose -p rvwasmtest up -d`.

## Build from source

    git submodule update --init                       # third_party/berkeley-softfloat-3
    docker compose --profile build build build        # the build image: LLVM is most of it
    docker compose --profile build run --rm build     # toolchain rv gpu stage pack -> ./dist
    docker compose --profile build run --rm build-debian    # the Debian disks (privileged; hours)
    docker compose --profile build run --rm build pack      # again, with the disks
    # the release disk on top (needs nativeqt's build: RV_NQ_SYSROOT_DIR=/path/to/its sysroot)
    RV_NQ_SYSROOT_DIR=... docker compose --profile build run --rm build-debian release
    RV_DISKS="release home" docker compose --profile build run --rm build pack
    docker compose up -d

Parallel jobs and memory: the image's LLVM is built with `RV_LLVM_JOBS` (default 6; each job can take over
1 GB, and with 8 jobs next to other work the 12 GB build VM ran out of memory). The build containers are
limited to `RV_BUILD_MEM` (default 8g) and run `RV_BUILD_JOBS` (default 6) jobs; on a VM shared with other
work, e.g. `RV_BUILD_MEM=4g RV_BUILD_JOBS=4 docker compose --profile build run --rm build`.

`docker/build.sh` steps (`docker compose --profile build run --rm build STEP...`):

| step | what |
|---|---|
| `toolchain` | the kernel (with `patches/kernel-*.diff` incl. the sound card's, config `rvwasm`; made again when the patches change), musl (`patches/musl-wasm64.diff`), BusyBox and the boot image's base, each only if not built yet; `kernel`, `musl`, `busybox` build one again |
| `rv` | riscv64 BusyBox (static and dynamic), the dynamic-linking and thread tests |
| `gpu` | virglrenderer's vrend for the page (`gpu/vrend/build.sh`, on a copy of `gpu/vrend`) |
| `stage` | `tools/stage.sh`: rv (wasm and native), the probes, hostnode, the window and GPU servers, the patched runtime, the boot image |
| `debian` | `tools/build-debian-image.sh`: root, x-root, kde-root, `dev kde`, `chromium kde`, `disk kde 8G daily`, `home` |
| `release` | the release disk: `tools/nq-bundle.sh` from `$NQ_SYSROOT`, `release-root`, `disk release 8G` (privileged, after `debian`) |
| `pack` | `tools/pack-www.sh /work/www /dist` |
| `all` | toolchain rv gpu stage pack (the default) |
| `shell` | a shell with all of that set up |

The `debian` step needs the `build-debian` service (privileged: debootstrap and chroot mount things and make
device nodes) and riscv64 registered in the host's binfmt_misc with the F flag, so that qemu works inside the
chroot: on Ubuntu/Debian `apt-get install qemu-user-static` on the host (check
`cat /proc/sys/fs/binfmt_misc/qemu-riscv64`: `flags: POF`). Without the Debian root, `stage` leaves out
Node's library for hostnode (it is taken out of the root's Node), so the desktop needs `debian`, then
`stage` again, then `pack`.

rvgpu (`?exec=webgpu`) is not built by the image (gpu/webgpu has its own build); `pack` takes it when it is
in `/work/www/rvgpu`.

### Paths

The scripts in `tools/` take their directories from the environment, with the build VM's as defaults, so
the VM works as before:

| variable | VM default | build image |
|---|---|---|
| `RVWASM_WORK` | `~/rvwasm` | `/work` |
| `RVWASM_MAIN` | `~/dev/rvwasm` (stage.sh: builds of other trees get their own directories) | `/src` |
| `GPUHOST` | `~/gpuhost` | `/work/gpuhost` |
| `EMSDK` | `~/emsdk` | `/opt/emsdk` |

## Disk, memory, time

Measured in the Lima VM (12 CPUs, 12 GB, arm64), 2026-10-10:

- run image: 5.9 GB, nearly all of it the `daily` disk (3.8 GB) and `home`; the page's files are 35 MB.
  The two containers use about 10 MB of memory each. The browser tab is what needs memory (several GB with
  the desktop).
- build image: 7.5 GB. LLVM took 26 minutes with 6 jobs, Emscripten under a minute; the rest is apt.
- `all` in the build container: about 5 minutes (kernel, musl and BusyBox with 10 jobs, then vrend, the stage
  and the pack with 4); `/work` is then 2.5 GB. The resulting rv passes the native suites in the container
  (`tools/busybox-check.sh` 62, `dyn-check.sh` 7, `thread-check.sh` 8).
- `debian`: not run in Docker yet. In the VM the roots and disks are ~25 GB and take hours.
