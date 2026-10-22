# Firefox

Status as of 2026-10-12, tested with Firefox 157.0.1 (the official Mozilla build, macOS arm64, headless, a throwaway
profile). **Firefox boots the machine to the Plasma desktop** once one runtime fix is in (patch 27 of
`tools/patch-runtime.py`, below). Before it, Firefox booted the kernel and reached a shell, and then `plasmashell`
died with `InternalError: out of memory` and no panel ever appeared. Everything else the page uses was already there
in Firefox; the "Firefox is not supported" of the landing page was not a missing specification.

## The one blocker, and why it was not a missing API

`plasmashell` is a 141 MB WebAssembly module (140,961,777 bytes). Every task of the machine is a Web Worker, and every
Worker compiled the program it runs from its own copy of the bytes (`wasm_load_executable` in linux-worker.js:
`WebAssembly.compile(bytes)`). plasmashell runs as several tasks (its threads), so the module was compiled 7 times in
one boot. V8 keeps one copy of the generated code for identical bytes in a process, so Chrome pays once.
SpiderMonkey has no such sharing, and it caps **all the executable code of a content process at 2044 MiB**
(`MaxCodeBytesPerProcess`, `js/src/jit/ProcessExecutableMemory.h`, 64-bit). The fourth compile failed:

    [Runner plasmashell (131111104)]: [probe] COMPILE FAILED 140961777: InternalError: out of memory
    [Main]: plasmashell (131108992) has a fatal signal and runs its program: its Worker is ended, ...

(and the respawned task failed the same way). The fix is patch **27** in `tools/patch-runtime.py`: in a browser whose
user agent says Firefox (or with `?share=1`; `?share=0` never), a program is keyed by its content (the hash patch 26
already has for libraries), the first Worker that compiles it posts the `WebAssembly.Module` to the main thread, and
every task is started with the modules the main thread has. A `Module` posted to another Worker shares its compiled
code in SpiderMonkey, so the code exists once. Measured on the booted desktop (`os.dl.log`): plasmashell compiled 1,
taken from the page's 15 times; the 1.5 MB `debian` wrapper compiled 1 time, taken 191 times; busybox `sh` 1 and 269.
Chrome is not changed (the flag is off there). Side effect in Firefox: every `exec` of a program used before is a
lookup instead of a compile.

A 2044 MiB code cap is also a limit to keep in mind: one copy of plasmashell plus what the JIT regions
(`__rv_jit_install`, one small module each) and libraries add must stay under it. The page's own accounting for that
(`jit_code`, 1.5 GiB budget in KiB) was written for V8's 4 GiB; see "What is left".

## What was tested, and the results

All of this ran headless in Firefox 157.0.1, one page at a time, in a temporary profile that was deleted afterwards.

| test | result |
|---|---|
| `/probe` (tools/www/probe.html), served locally with the site's COOP/COEP headers | every test passes. Details below |
| `https://plasmaontheweb.com/desktop`, the live page, as one visitor | kernel boots (first kernel line about 110 s in on a cold disk), shell prompt at about 170 s, WebGL2 window server, the vrend GPU Worker and the WebCodecs decoder start (`vdec: the page decodes H.264, VP8, VP9, AV1 4096x2304 (hardware)`), then plasmashell crashes with the out-of-memory error above. No panel. |
| this branch's page and runtime, local server, disk and everything else from the live site | desktop with panel, launcher, clock; no crash; plasmashell compiled once |
| same with `?mem=8192` (the memory object really is 8 GiB), then clicks on the panel and the Fullscreen button | no crash; the panel's fourth launcher opened its web browser (luakit, a riscv64 program through the JIT, "Error" page because the network was off); Fullscreen worked (`document.fullscreenElement` set, the `keyboardLock: "browser"` request accepted). A click on the Dolphin launcher 20 s after the panel appeared opened nothing within 90 s: not repeated, so not known whether that is startup timing; check it in the owner's run |
| the probe's extra buttons | 400 Workers up in 106 ms; memory grown to the full 16 GiB; the kernel compiles in 2.4 s (10,994 exports, `wasm_mem_pages_max` present); AudioWorklet runs once the context is allowed |

### The probe in Firefox 157 (all PASS)

shared memory64 with a 16 GiB maximum accepted (descriptor key `address`), grows to 8 GiB; a Worker sees a write at
5 GiB; a wasm64 module over the shared memory instantiates in a Worker; 64 Workers up in 30 ms; `Atomics.wait` in a
Worker, nested Workers and synchronous XHR in a Worker work; OPFS `createSyncAccessHandle` writes 64 MiB in 65 ms and a
second handle on the same file fails with `NoModificationAllowedError` (what disk-worker.js expects); quota 10 GiB;
WebGL2 on the page and in a Worker on an OffscreenCanvas (renderer string "Apple M1, or similar": Firefox hides the
exact GPU), shared-memory views accepted by `texSubImage2D`, `bufferSubData`, `readPixels` and `getBufferSubData`,
fences, the ImageBitmap round trip; WebCodecs `VideoDecoder` H.264, VP8, VP9, AV1 (hardware and any), HEVC not; AudioWorklet
loads from a Blob and the machine's shared Memory arrives shared; every wasm feature validates (threads, bulk memory,
sign extension, memory64, SIMD, relaxed SIMD, tail calls, legacy exceptions, exnref); `performance.now()` steps of 20 us
under cross-origin isolation. Missing: `navigator.deviceMemory`, `navigator.keyboard` (`getLayoutMap`, `lock`),
`navigator.userAgentData`. JSPI is present; WebGPU has `navigator.gpu` but returned no adapter in headless mode.

## Findings table

Severity: **blocks** = does not boot; **degrades** = boots, something is worse or missing; **cosmetic**.
"Firefox" says what Firefox has, with its source. Status: fixed here, left, or nothing to do.

| # | Chrome-ism or assumption | Where | Firefox | What we did / should do | Severity |
|---|---|---|---|---|---|
| 1 | Each Worker compiles its own copy of a program; V8 shares the code of identical modules, SpiderMonkey does not and caps process code at 2044 MiB | linux-worker.js `wasm_load_executable` | `ProcessExecutableMemory.h`; reproduced: 7 compiles of plasmashell, the 4th fails | **Fixed**: patch 27, the Module is compiled once and posted to every Worker (Firefox only) | **blocks** (the desktop) |
| 2 | `navigator.deviceMemory` sizes the machine's memory (`mem=`) | dev.html | absent ([MDN compat data](https://github.com/mdn/browser-compat-data/blob/main/api/Navigator.json): Firefox false) | Left: Firefox gets the kernel's 3 GiB unless `?mem=MiB`. See "What is left" | degrades |
| 3 | `navigator.userAgentData` for the Mac check | display.js `isMac` | absent | nothing to do: `navigator.platform` is tried first and is enough | cosmetic |
| 4 | `performance.measureUserAgentSpecificMemory` | not used anywhere in tools/www or the runtime | n/a | nothing to do | none |
| 5 | JSPI (`WebAssembly.Suspending`/`promising`) | only rvgpu.wasm (`?exec=webgpu`); vrend (WebGL2) has none: `gpu/vrend/build.sh` is plain Emscripten, no JSPI, no pthreads | JSPI from Firefox 153 (compat data) | **Fixed**: gpu-exec.js falls back to vrend when `navigator.gpu` exists but JSPI does not | degrades (only for `?exec=webgpu`) |
| 6 | WebGPU | `?exec=webgpu` only; the default GPU path is WebGL2 | Windows 141+, Apple Silicon macOS 147+ per the release notes; Linux unclear; no adapter in the headless run | nothing to do; the page already falls back to vrend when there is no device | none |
| 7 | `Atomics.waitAsync` (the GPU executor and the video decoder wait on doorbells with it) | gpu-exec.js `run`, vdec-worker.js `run` | from Firefox 145 ([release notes](https://developer.mozilla.org/en-US/Firefox/Releases/145)) | **Fixed** for older Firefox (ESR 140): polls on a timer when it is missing | degrades on Firefox below 145 |
| 8 | Shared 64-bit memory with a 16 GiB maximum: `new WebAssembly.Memory({initial, maximum: 0x40000n, shared: true, address: "i64"})` | linux.js | Memory64 from Firefox 134 ([SpiderMonkey blog](https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html)); limit `ArrayBufferObject::ByteLengthLimit` is 16 GiB on 64-bit (`js/src/vm/ArrayBufferObject.h`); the descriptor key is `address` in ESR 140 and later, `index` in ESR 128 (`js/src/wasm/WasmJS.cpp`) | probe passes in 157. Firefox 134 to 139 may still say `index`: left, they are out of date | blocks on Firefox < 140 |
| 9 | Worker count (one per task, plus spares, CPUs, disk fetchers) | linux.js | probe: 64 Workers in 30 ms, 400 in 106 ms; a desktop boot has about 70 | `dom.workers.maxPerDomain` was not hit | none seen |
| 10 | `Atomics.wait` on the main thread | none: every wait is in a Worker (disk-worker.js, linux-worker.js, gpu-stub.js) | n/a | nothing to do | none |
| 11 | Growing shared memory past 4 GiB, a `Uint8Array` view on it above 4 GiB | linux.js, display.js, audio worklet | probe: grows to 8 GiB, atomics at 5 GiB from a Worker work; the buffer limit is 16 GiB | nothing to do | none |
| 12 | `WebAssembly.compileStreaming` needs `application/wasm` | dev.html | the server sends it (Caddyfile; the probe prints the header it sees) | nothing to do | none |
| 13 | OPFS `createSyncAccessHandle()` (no arguments), exclusive per file | disk-worker.js | Firefox 111; the `mode` option is not supported (and is not used) | probe passes. Private windows: OPFS is not available in Firefox private browsing as far as is known; the disk worker already falls back to memory only and says so in the log. Not tested here | degrades in private windows |
| 14 | Storage quota for a 3.9 GB disk plus the home disk | disk-worker.js | best-effort quota is the smaller of 10% of the disk and 10 GiB per site ([MDN](https://developer.mozilla.org/en-US/docs/Web/API/Storage_API/Storage_quotas_and_eviction_criteria)); a computer with a disk under about 64 GB has less than the 5 GB the machine wants | the probe prints the quota; left: the machine's own error message when the browser refuses a write | degrades on small disks |
| 15 | `navigator.storage.persist()` at page load | dev.html | Firefox asks the person with a prompt (MDN, same page) | **Fixed**: in Firefox the request waits for the first click or key; Chrome unchanged | cosmetic |
| 16 | WebCodecs `VideoDecoder` in a Worker, codec strings `avc1.640033`, `vp09.00.51.08`, `av01.0.13M.08`, `hev1.1.6.L156.B0` | vdec-worker.js | Firefox 130 desktop; 157 supports H.264, VP8, VP9, AV1; HEVC not | nothing to do: the probe in the Worker decides per codec, an unsupported codec decodes in software | degrades for HEVC |
| 17 | AudioWorklet with the machine's shared memory | audio.js, audio-worklet.js | works (probe): the Memory arrives shared; the context starts suspended until a click, as in Chrome | nothing to do | none |
| 18 | `navigator.clipboard.readText/writeText`, Ctrl-V through the paste event | display.js | `writeText` Firefox 63, `readText` 125 (it shows a small paste button) | nothing to do: the paste event is tried first and works | none |
| 19 | `navigator.keyboard.getLayoutMap()` guesses the host layout; `navigator.keyboard.lock()` in fullscreen | display.js | both absent (compat data: Firefox false) | lock **fixed**: Firefox 151 and later take it in the fullscreen request (`keyboardLock: "browser"`); layout: **left**, a Firefox user starts with US and picks a layout in the machine's bar or opens `/desktop?kbd=de` | degrades |
| 20 | Pointer Lock | not used | n/a | nothing to do | none |
| 21 | Fullscreen with `navigationUI: "hide"` | display.js | supported | nothing to do | none |
| 22 | COOP/COEP | docker/Caddyfile sends `same-origin` and `require-corp` on every answer | Firefox supports both; the probe shows `crossOriginIsolated` true. `credentialless` and `Document-Isolation-Policy` are not sent and not needed | nothing to do | none |
| 23 | WebGL2 with views of the shared memory (`texSubImage2D`, `bufferSubData`, `readPixels`) | display.js, gpu-exec.js | Firefox's IDL has `[AllowShared]` on these (`dom/webidl/WebGL2RenderingContext.webidl`); probe passes | nothing to do | none |
| 24 | The landing page's own check: `/Chrome\//.test(navigator.userAgent)`, text "Firefox, Safari ... are not supported" | site/index.html | n/a | **Fixed in the script**: it asks the browser for shared memory, 64-bit WebAssembly memory and the file system, and says "Firefox is experimental" instead of "not supported". The visible texts (the FAQ, the requirements list, the structured data) still say Chrome only: the owner decides when to change them | cosmetic |
| 25 | The page's patches of `Worker.prototype.onmessage`, `Worker`, `requestAnimationFrame`, `Atomics.notify` (watchdog) | dev.html | work (the machine booted) | nothing to do | none |

## What was fixed (branch `firefox`)

- `tools/www/probe.html`: the probe, served at `/probe` (Caddyfile route, the rate-limit zones, `tools/stage.sh`,
  `docker/patch-dist.sh`, `tools/deploy.sh`, `tests/caddy-limits.sh` know it).
- `tools/patch-runtime.py` patch 27: programs compiled once per page in Firefox.
- `tools/www/gpu-exec.js`: `Atomics.waitAsync` fallback; `?exec=webgpu` needs JSPI as well as `navigator.gpu`.
- `tools/www/vdec-worker.js`: `Atomics.waitAsync` fallback.
- `tools/www/dev.html`: `storage.persist()` waits for a gesture in Firefox.
- `tools/www/display.js`: Firefox's keyboard lock (fullscreen option).
- `site/index.html`: the landing page's browser check asks for features. (Its script never reached the check: the nav
  code called `querySelector` on the external "source" link's address, which throws in every browser and ended the script, so
  the compat note and the carrying over of `?mem=` to the boot links never ran. Fixed when shipped: only `#...` links are looked up.)
- Shipping (2026-10-13): the live dist's linux.js/linux-worker.js were built before patch 26, so `docker/patch-dist.sh` now runs
  `tools/patch-runtime.py` on the dist (each patch checks its marker) and copies display.js. Patch 26 is inert without `?dl=1`.

Nothing changes in Chrome: each of these is behind a feature test or the Firefox user agent, and the probe and
patch 27 add files or code that Chrome never runs.

## What is left

- **Memory size.** Firefox has no `navigator.deviceMemory`, so the page gives the machine the kernel's 3 GiB (the
  live-site boot said "memory not told"). `?mem=8192` boots to the desktop in Firefox 157, so the page could use
  `navigator.hardwareConcurrency` for a guess, as it already does for Chrome's 8 GB case. Not done: a larger machine is
  a larger tab, and a real long session's `about:memory` should be looked at first.
- **Code budget.** `jit_code` (linux.js, 1.5 GiB of translated code) was made for V8's code space. In Firefox the
  limit is 2044 MiB for everything in the process, plasmashell's own code included, and `about:memory` should be looked
  at after a long session with the JIT busy (rv's regions are small modules, one each). If Firefox runs out, the page
  sees `__rv_jit_install` failing and rv goes on interpreting.
- **Keyboard layout detection** (item 19).
- **Firefox private windows** (item 13) and **small disks** (item 14): not tested, behaviour described above.
- **Visible text** of the landing page (item 24) and the structured data's `browserRequirements`.
- **Other platforms**: only macOS arm64 was run. Windows and Linux Firefox have other GPU and memory behaviour.
- **Performance** against Chrome was not measured.

## Engine bugs

None found. Two Firefox behaviours were involved in the blocker; neither is a bug, so nothing is drafted for Bugzilla
as a defect. If the owner wants to ask for them anyway, the honest framing is an enhancement request: (1) "WebAssembly:
share compiled code between identical modules compiled in different Workers of one process, as V8's native module
cache does" (repro: compile the same 140 MB module in 7 Workers, the fourth `WebAssembly.compile` rejects with
`InternalError: out of memory` because `MaxCodeBytesPerProcess` is 2044 MiB; posting one Module to the others works and
is what patch 27 does); (2) the error text says "out of memory" where "executable code limit reached" would have saved
the hour spent on it. Also seen, cause unknown and not Firefox-specific as far as is known: 13 of the live disk's
fetch-ahead requests to `disk.plasmaontheweb.com` ended `NS_BINDING_ABORTED` in the live-site boot (the machine
completed its reads; likely the disk worker cancelling a fetch it no longer needed).

## Owner's test steps for real Firefox

1. Open `https://plasmaontheweb.com/probe` (after the deploy of this branch; until then open the local copy). The
   verdict at the top says whether anything blocks the boot. Press **Copy report** and keep it. Press the buttons at
   the bottom once: **More workers (400)**, **Memory to 16 GiB**, **Compile the kernel (7 MB)**, and **Audio test** (that
   one needs the click).
2. Open `/desktop`. Time: the first kernel line, the shell prompt, the panel. Open Konsole, Dolphin and Kate from the
   launcher; type in Konsole; paste text from the host with Ctrl-V (Firefox may show a "Paste" button).
3. Press **Fullscreen** and try Alt-Tab and Escape.
4. `about:memory`, "Measure": note the content process for the tab (the size of "wasm" code and the JS memory).
   Look at the Web Console for errors.
5. Report: the probe report, how long the boot took, what crashed or was missing, Firefox version and operating system,
   whether the tab was a private window, and the disk size of the computer.

## Native programs in Firefox: the code cap, measured and kept (2026-10-14, runtime item 31)

The first try (sonnet-nativedefault2, below in the git history) saw `uncaught exception: out of memory` when Konsole, Dolphin or Kate
was started with `?dl=1` and kept the Firefox gate. Cause found by measuring the engine's own account, not by the page's:

**The cap.** SpiderMonkey reserves ONE region of executable memory per process, `MaxCodeBytesPerProcess` = 2044 MiB (js/src/jit/
ProcessExecutableMemory.h), carved in blocks of `ExecutableCodePageSize` = 64 KiB (first fit from a cursor, so a big module needs
ONE contiguous run); every wasm code segment of every Worker of the tab comes out of it, and a failed block is reported to the page
as `InternalError: out of memory`. Code goes back when the last reference to its Module/Instance has been collected (the engine
asks the main thread for a collection once, when a big allocation fails: `OnLargeAllocationFailure`).

**Reading it.** `vmmap -pages PID` of the tab's content process shows the region (a `rwx/rwx VM_ALLOCATE` of 130816 pages of 16 KiB);
its RESIDENT + SWAPPED pages are the code committed (macOS compresses idle pages: they stay in SWAPPED). `tests/wasmdyn/
native-firefox.js` prints it as `EXEC CODE` at the panel and after each program, and as `exec code` while one starts.
`--gauge` asks the engine instead (tiny modules cost 128 KiB each, so the count that compiles before the error is the free space;
and the largest module that compiles). about:memory's `non-heap/code/wasm` is no account of the process: each Worker that holds a
shared Module reports it again.

**What costs what** (probe pages, fresh process, modules compiled until the engine said no): a wasm module takes at least TWO blocks
(its code and its stubs): 128 KiB for the smallest. The JIT's batches of translated regions (rv/jit.c, runtime item 29), real ones
captured from the machine: 12-24 KB of wasm 128 KiB, 30-55 KB 256 KiB, 60-76 KB 268 KiB, 90-110 KB 332 KiB (about 4.4 times the
wasm, in blocks of 64 KiB plus 64 KiB). Libraries and programs: about 4.5 to 5 times their wasm size (plasmashell 134 MB of wasm,
the 28 preloaded libraries 52 MiB, the libraries a Konsole start compiles 47 MiB).

**What filled it.** Not the native libraries. The JIT: every Worker kept every batch Module it ever compiled in its `jit_shared`
map (item 29, for sharing by content), for good, so when rv let go of regions (item 16) nothing was freed; and the budget for
translated code (1536 MiB, counted as 3.5 times the wasm) was written for V8's 4 GiB. With `?dl=0` the same boot logged
`__rv_jit_install: out of memory` 2755 times in 100 s, with the cap at half of what the page's own count said. Control runs,
`?dl=1&jitshare=0&jitcode=128`: Konsole, Dolphin and Kate open, 0 errors.

**Item 31 (Firefox only; Chrome runs none of it):**
1. batches are not shared by content or kept (`jit_share` off in Firefox; `?jitshare=1` brings it back), so rv's evictions free;
2. the budget for translated code is 192 MiB (`?jitcode=MB`), counted as the engine takes it (the table above);
3. a library or program that finds no code space ("out of memory" from its compile) is tried again every 250 ms for up to 6 s
   (`[Firefox]: no code space ...` in the machine's log, `code_space_retries` in the harness): a program starting compiles
   in several Workers at once and the other Workers' garbage goes only at their next collection. Without it one Konsole launch in
   three or four ended with `ld-wasm: thread N could not load log entry M (-1)` (a thread's compile of a 2.8 MB library: the process
   dies when a thread cannot load a library); with it 0 of 10;
4. the Firefox gate of `rv_dl_wanted` is gone: the rule is the same in every browser (the default itself, `DEFAULT_ON`, is a separate
   switch and was OFF at this writing).

**Results** (headless Firefox 157.0.1, macOS arm64, the live dist of 2026-10-14 with jitmem and storage plus item 31, local server,
`tests/wasmdyn/native-firefox.js`, default memory):

| run | result |
|---|---|
| default URL (native), Konsole, Dolphin, Kate, 180 s hold | panel 16 s; Konsole 83 s (cold, libraries compile as asked), Dolphin 8.7 s, Kate 15 s; 0 errors, 0 out of memory, display alive, all three windows after the hold; code 1.2 GiB at the panel, 1.75-1.8 GiB with the three open (of 2044 MiB) |
| Konsole alone, repeated, on a fresh profile each | before the retry 5 launches of 18 without a window; with it 0 of 10, 23-34 s |
| `?dl=0`, emulated Konsole | opens (145 s on a busy machine), `__rv_jit_install` out of memory 0 (2755 before) |
| all of Konsole, Dolphin, Kate, System Settings, Okular, Gwenview, Ark, KCalc, one after the other (before the retry) | the other seven 6 s each, code 1.2-1.4 GiB |

**What is left.** The margin: 250 MiB with three programs open, code that stays: plasmashell (about 600 MiB), every library a program
used (kept as Modules in the page, `dl_modules`, and in the Workers' copies of that map, for the page's life), the programs'
own modules (patch 27). Each further program only adds its own few libraries, but a long session with many different programs will
get there. The levers, in the order they would be taken: keep only the last few programs' modules and drop the rest from
`dl_modules` (a library in use stays alive through its instances; the next start of a program compiles it again), a smaller
preload set in Firefox, `?dlbg=1` is the opposite (it keeps more) and stays off. Not done because the measured margin does not need
it yet, and not measured: Firefox on Windows/Linux, a launcher/panel-started program in Firefox (`--via panel` of the harness
started nothing in one try, with the bucket unreachable at the time: not a result).
