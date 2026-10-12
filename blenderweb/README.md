# blenderweb: Blender on the host's GPU, as a window of the desktop

Debian's riscv64 Blender runs in the machine, but only with software
OpenGL (llvmpipe, under the emulator): its 3D viewport never draws, and
WebGL2 passed through cannot give it OpenGL 4.3. Puter has compiled
Blender 5 itself to WebAssembly with a WebGPU backend
([blender-wasm](https://github.com/HeyPuter/blender-wasm)): the real
interface and EEVEE, on the host's GPU. That build runs here natively in
the browser, next to the machine, and is shown as a window of the
machine's desktop that opens and saves the machine's files.

    blender-gpu [FILE.blend]      in a terminal of `desktop wayland` (FILE in the home directory)
    Blender (GPU)                 in the launcher (tools/x/xdg/data/applications/rv-blender-gpu.desktop)

## The parts

- `guest/boot.js` (staged to `/run/wlserver/blenderweb`, started by
  `guest/blender-gpu`): a hostnode program. It makes a Wayland window at
  the window server whose app id is `rvembed-ID.rv-blender-gpu`, one grey
  pixel stretched to the window's size (wp_viewporter); asks the page for
  Blender on the BroadcastChannel `rv-blenderweb-ID` (ID: the page load's, `rvtab=ID` on the kernel's command line; with a mark in the
  machine's memory, as hostopen does: only this machine's page takes it);
  and serves the home directory to Blender (stat, list, read, write,
  mkdir, unlink, rename; openat2 with RESOLVE_BENEATH, so nothing outside
  the home directory, through `..` or a symbolic link, is reached). Its
  header has the messages.
- `tools/www/blenderweb.js` (the machine's page): makes an iframe for it
  and hands it to display.js (`rvDisplay.embed`), which lays it over the
  window every frame: above the window's own frame, under the windows
  above it (clipped to what they leave), hidden with it (minimized,
  shaded, another desktop). So the window moves, resizes, stacks,
  minimizes and is switched to like any other; the iframe is its inside.
  It also passes the file operations between the frame and the program.
- `www/` (Blender's page, from another origin): `main.js` is Puter's
  `demo/src/main.js`, changed (its header says how): Blender starts at
  once, the machine's home is mounted at `/root` through Puter's own
  provider backend (read and write), File > Open and File > Save open
  Blender's file browser there, clicks bring the window forward, Alt+Tab
  goes to the desktop, Blender's title becomes the window's, closing asks
  first.
- `serve.py`: serves `www/` and the release, threaded, with the headers an
  iframe of a cross-origin-isolated page needs (COEP require-corp, CORP
  cross-origin); only pages on localhost may frame it.
- `fetch-release.sh`: Puter's release (1.0.0, `blender7-1.tar.gz`, checked
  by its SHA-256), unpacked and decompressed into `~/rvwasm/blender-web`
  in the build VM. Their own page is not used (it loads an analytics
  script and Google's fonts): nothing loads from outside this computer.

## Running it

In the build VM, once: `blenderweb/fetch-release.sh`; then the server, in
its own tmux session like the dev server's:

    tmux new-session -d -s blender-www "python3 ~/dev/rvwasm/blenderweb/serve.py"

It listens on 127.0.0.1:8040 (Lima forwards it to the Mac). The machine's
page on `localhost:8000` frames `http://127.0.0.1:8040/` (on
`127.0.0.1:8000`, `http://localhost:8040/`): another site, so Chrome gives
Blender a renderer process of its own (its memory, its 4 GiB of compiled
code), not the machine's tab's. `?blenderweb=PORT` on the page's address
names another port.

Blender's page needs WebGPU. Chrome on a Mac has it; headless Chrome for
Testing too (Metal).

## Why another site, and the public setup

The frame must be another *site* (scheme plus registrable domain), not just another origin: the reason is
process isolation. Blender's engine is 145 MB of Wasm and V8 limits the compiled code a process may hold, so the
machine's tab and Blender must not share a renderer process. Chrome separates processes by site; a subdomain
(blender.plasmaontheweb.com) is another origin but the same site and shares the page's process (the
Origin-Agent-Cluster header is only a hint, "may not always be honored"). It is not COOP/COEP (the frame only has
to be compatible: COEP require-corp + CORP cross-origin) and not storage (OPFS/IndexedDB are per origin anyway).

Public: the page reads `"blender"` (an https address) from `/rv-site.json` (Caddy: `RV_BLENDER_URL` in the server's
`.env`). Empty or absent: no Blender; a program asking for it is refused and the page shows a note, nothing is
fetched from 127.0.0.1 (that default is for pages on localhost only). Caddy serves `/blender/` from
`~/rvwasm/blender-web` (`tools/deploy-blender.sh`; `RV_BLENDER_ANCESTORS` = the pages allowed to frame it). Now:
`https://SERVER-IP.sslip.io/blender/` (sslip.io is another registrable domain than plasmaontheweb.com, with
a certificate already). To use a domain of your own instead: buy or use a second registrable domain, add an A
record to the server's address, add it to `RV_ALT_DOMAINS`, and set `RV_BLENDER_URL=https://THAT-DOMAIN/blender/`
and `RV_BLENDER_ANCESTORS` (then `docker compose up -d --no-deps caddy`).

## Files

The home directory is `/root` in Blender as in Debian. File > Open shows
it; File > Save (Ctrl+S) of a new file opens the save browser there; a
file opened from it, or named on the command line, is saved back in
place (Blender writes `NAME.blend@` and renames it, keeping the old one as
`NAME.blend1`, as on any disk). Every read and write goes through the
page to `boot.js` in the machine: whole files, as the provider backend
works (a file is read when it is opened and written when it is closed).
Blender's own settings and its shader cache are kept in the frame's
origin (OPFS and IndexedDB of `127.0.0.1:8040`), not in the machine.

## License

Blender and Puter's build are under the GNU GPL, version 2 or later.
`fetch-release.sh` writes `SOURCE.txt` next to the release, saying where
the source of exactly that build is (the harness, and Blender as patched
for the web at the commit the harness names). `www/main.js` is derived
from Puter's demo and is under the same license.
