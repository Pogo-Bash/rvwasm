/* Blender (Puter's blender-wasm, release 1.0.0) as a window of the machine's desktop.
 *
 * This is Puter's demo/src/main.js (https://github.com/HeyPuter/blender-wasm, GPL-2.0), changed for here:
 *
 * - No download screen or Launch button: Blender starts as soon as the engine and its files are in. They come
 *   from blenderweb/serve.py on this computer, already decompressed (fetch-release.sh), so there is no zstd
 *   decoder here, and the engine is compiled while it streams in.
 * - Files: the machine's home directory (/root in Debian) is mounted at /root in Blender's file system, through
 *   the same WasmFS provider backend Puter's demo uses for a dropped folder (provider_backend.cpp +
 *   provider-fs.js, compiled into their blender.js), read and written: every call goes to the machine's page
 *   (the parent of this frame) with postMessage, from there to the program in the machine that made this
 *   window (/run/wlserver/blenderweb/boot.js), which does it with the kernel's system calls. File > Open and
 *   File > Save open Blender's own file browser there (Puter's "folder picker" hooks, answered without one).
 * - The window: the parent is told when the person clicks here (the window comes to the front), Alt+Tab and
 *   the Meta key go to the desktop, and Blender's title (the file's name) becomes the window's.
 * - Closing the window asks first (what is not saved would be lost).
 *
 * Nothing is sent anywhere but to the parent page (the target origin of every postMessage is the parent's,
 * from the address of this frame, and only the parent's messages are taken).
 */

const splashEl = document.getElementById("splash");
const statusEl = document.getElementById("status");
const fillEl = document.getElementById("fill");
const logEl = document.getElementById("log");
const canvas = document.getElementById("canvas");
const askEl = document.getElementById("ask");

/* ---- the parent: the machine's page. Its origin comes in this frame's address (#parent=ORIGIN). ---- */
const params = new URLSearchParams(location.hash.slice(1));
const parentOrigin = params.get("parent") || "";
const toParent = (message) => {
  if (window.parent !== window && parentOrigin) {
    window.parent.postMessage({ rv: "blenderweb", ...message }, parentOrigin);
  }
};

/* ---- the log: the start's lines on the splash screen, and the last ones kept for the parent ---- */
const MAX_LINES = 300;
const lines = [];
const nativeConsole = { log: console.log.bind(console), warn: console.warn.bind(console), error: console.error.bind(console) };
const show = (level, args) => {
  const text = args.map((a) => (a instanceof Error ? a.stack || a.message : typeof a === "string" ? a : (() => {
    try { return JSON.stringify(a); } catch (e) { return String(a); }
  })())).join(" ");
  lines.push("[" + level + "] " + text);
  if (lines.length > MAX_LINES) {
    lines.shift();
  }
  if (!splashEl.classList.contains("done")) {
    logEl.textContent = lines.slice(-40).join("\n");
    logEl.scrollTop = logEl.scrollHeight;
  }
};
console.log = (...a) => { show("log", a); nativeConsole.log(...a); };
console.warn = (...a) => { show("warn", a); nativeConsole.warn(...a); };
console.error = (...a) => { show("error", a); nativeConsole.error(...a); };
const log = (m) => console.log(m);
window.__blenderwebLog = () => lines.slice();

const setStatus = (text, fraction, error) => {
  statusEl.textContent = text;
  statusEl.classList.toggle("error", !!error);
  if (fraction !== undefined) {
    fillEl.style.width = Math.round(Math.max(0, Math.min(1, fraction)) * 100) + "%";
  }
  toParent({ type: "status", text, error: !!error });
};
const fail = (text) => {
  setStatus(text, undefined, true);
  splashEl.classList.remove("done");
  toParent({ type: "failed", why: text });
};

/* ---- requests to the machine (through the parent), and their answers ---- */
let nextId = 1;
const pending = new Map();
const ask = (op, args = {}, transfer = []) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  if (window.parent === window || !parentOrigin) {
    pending.delete(id);
    reject(new Error("no machine page around this frame"));
    return;
  }
  window.parent.postMessage({ rv: "blenderweb", type: "fs", id, op, ...args }, parentOrigin, transfer);
});

let init = null;		// { open, home } from the parent
let initArrived = null;
const initPromise = new Promise((resolve) => {
  initArrived = resolve;
});

window.addEventListener("message", (event) => {
  if (event.source !== window.parent || event.origin !== parentOrigin) {
    return;
  }
  const m = event.data;
  if (!m || m.rv !== "blenderweb") {
    return;
  }
  if (m.type === "fs-answer") {
    const p = pending.get(m.id);
    if (p) {
      pending.delete(m.id);
      if (m.ok) {
        p.resolve(m.value);
      } else {
        const error = new Error(m.error || "failed");
        error.errno = m.errno || 5;
        p.reject(error);
      }
    }
  } else if (m.type === "init" && !init) {
    init = { open: typeof m.open === "string" ? m.open : null, home: typeof m.home === "string" ? m.home : "/root" };
    initArrived(init);
  } else if (m.type === "focus") {
    if (document.activeElement !== canvas) {
      canvas.focus({ preventScroll: true });
    }
  } else if (m.type === "close") {
    if (!window.Module || !blenderUp) {
      toParent({ type: "quit" });	// (nothing to lose yet)
    } else {
      askEl.classList.add("shown");
      document.getElementById("ask-no").focus();
    }
  } else if (m.type === "mem") {
    // What Blender's memory is: its Wasm memory, and the JavaScript heap (Chrome's performance.memory).
    const M = window.Module || {};
    const heap = M.HEAPU8 ? M.HEAPU8.length : M.wasmMemory ? M.wasmMemory.buffer.byteLength : null;
    const js = performance.memory ? performance.memory.usedJSHeapSize : null;
    toParent({ type: "mem", wasm: heap, js });
  } else if (m.type === "log") {
    toParent({ type: "log", lines: lines.slice(-(Number(m.count) || 50)) });
  }
});
document.getElementById("ask-no").onclick = () => {
  askEl.classList.remove("shown");
  canvas.focus({ preventScroll: true });
};
document.getElementById("ask-yes").onclick = () => toParent({ type: "quit" });

// The parent is told this page is here (again, until it answers: it may still be setting up). Opened by
// itself (to try it), there is no machine: Blender starts without the home directory.
if (window.parent === window) {
  init = { open: null, home: null };
  initArrived(init);
} else {
  let tries = 0;
  const hello = () => {
    if (init || tries++ > 120) {
      return;
    }
    toParent({ type: "hello" });
    setTimeout(hello, 250);
  };
  hello();
}

/* ---- the machine's home as an FsProvider (stat, readdir, readFile, writeFile, mkdir, unlink, rename) for
 * the WasmFS provider backend. Paths are relative to the mount point. A directory's listing brings every
 * entry's kind and size along, kept for a few seconds: the backend stats each entry of a listing right after
 * it (provider-fs.js), and Blender's file browser lists often. ---- */
const KEEP_MS = 3000;
const statCache = new Map();	// path -> { at, st: { size, isDir } | null }
const clean = (p) => p.split("/").filter((x) => x && x !== ".").join("/");
const parentOf = (p) => {
  const at = p.lastIndexOf("/");
  return at < 0 ? "" : p.slice(0, at);
};
const forget = (p) => {
  statCache.delete(p);
  statCache.delete(parentOf(p) + "/");	// (the listing of its directory)
};
const homeProvider = {
  async stat(path) {
    const p = clean(path);
    const c = statCache.get(p);
    if (c && performance.now() - c.at < KEEP_MS) {
      return c.st;
    }
    const st = await ask("stat", { path: p });
    statCache.set(p, { at: performance.now(), st });
    return st;
  },
  async readdir(path) {
    const p = clean(path);
    const entries = await ask("list", { path: p });
    const now = performance.now();
    for (const e of entries) {
      statCache.set(p ? p + "/" + e.name : e.name, { at: now, st: { size: e.size, isDir: e.isDir } });
    }
    return entries.map((e) => e.name);
  },
  async readFile(path) {
    const data = await ask("read", { path: clean(path) });
    return data instanceof Uint8Array ? data : new Uint8Array(data);
  },
  async writeFile(path, data) {
    const p = clean(path);
    forget(p);
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    await ask("write", { path: p, data: bytes }, [bytes.buffer]);
    forget(p);
  },
  async mkdir(path) {
    const p = clean(path);
    forget(p);
    await ask("mkdir", { path: p });
  },
  async unlink(path) {
    const p = clean(path);
    forget(p);
    await ask("unlink", { path: p });
  },
  async rename(from, to) {
    const a = clean(from), b = clean(to);
    forget(a);
    forget(b);
    await ask("rename", { path: a, to: b });
  },
};
const HOME_MOUNT = 1;	// (0 is Blender's own files, the tar below)

/* ---- Blender's own files: assets.tar, indexed where it lies (Puter's code: views into the one buffer, no
 * copies), as the provider for /assets ---- */
const textDecoder = new TextDecoder();
function parseTarString(bytes, start, length) {
  let end = start;
  const max = start + length;
  while (end < max && bytes[end] !== 0) end++;
  return textDecoder.decode(bytes.subarray(start, end));
}
function parseTarSize(bytes, start) {
  const first = bytes[start];
  if (first & 0x80) {
    let size = first & 0x7f;
    for (let i = start + 1; i < start + 12; i++) size = (size * 256) + bytes[i];
    return size;
  }
  const raw = parseTarString(bytes, start, 12).trim();
  return raw ? Number.parseInt(raw, 8) : 0;
}
function parsePax(data) {
  const text = textDecoder.decode(data);
  const out = {};
  let i = 0;
  while (i < text.length) {
    const space = text.indexOf(" ", i);
    if (space < 0) break;
    const length = Number.parseInt(text.slice(i, space), 10);
    if (!Number.isFinite(length) || length <= 0) break;
    const record = text.slice(space + 1, i + length - 1);
    const eq = record.indexOf("=");
    if (eq >= 0) out[record.slice(0, eq)] = record.slice(eq + 1);
    i += length;
  }
  return out;
}
function tarEntryName(bytes, offset) {
  const name = parseTarString(bytes, offset, 100);
  const prefix = parseTarString(bytes, offset + 345, 155);
  return prefix ? `${prefix}/${name}` : name;
}
function isEmptyBlock(bytes, offset) {
  for (let i = offset; i < offset + 512; i++) if (bytes[i] !== 0) return false;
  return true;
}
function addToTree(dirs, parts, isDir) {
  let dir = "";
  for (let i = 0; i < parts.length; i++) {
    const isLast = i === parts.length - 1;
    let children = dirs.get(dir);
    if (!children) { children = new Set(); dirs.set(dir, children); }
    children.add(parts[i]);
    if (isLast && !isDir) break;
    dir = dir ? `${dir}/${parts[i]}` : parts[i];
    if (isLast && !dirs.has(dir)) dirs.set(dir, new Set());
  }
}
function indexTar(bytes) {
  const files = new Map();
  const dirs = new Map([["", new Set()]]);
  let offset = 0;
  let pax, longName;
  while (offset + 512 <= bytes.length && !isEmptyBlock(bytes, offset)) {
    const type = String.fromCharCode(bytes[offset + 156] || 0);
    const size = parseTarSize(bytes, offset + 124);
    const dataStart = offset + 512;
    const dataEnd = dataStart + size;
    if (dataEnd > bytes.length) throw new Error("asset-fs: truncated tar");
    const data = bytes.subarray(dataStart, dataEnd);
    const name = (pax?.path ?? longName ?? tarEntryName(bytes, offset)).replace(/^\.\//, "");
    pax = undefined; longName = undefined;
    const parts = name.split("/").filter(Boolean);
    if (name.startsWith("/") || parts.includes("..")) throw new Error(`asset-fs: unsafe path ${name}`);
    if (type === "x") {
      pax = parsePax(data);
    } else if (type === "L") {
      longName = parseTarString(data, 0, data.length);
    } else if (parts.length && (type === "0" || type === "\0" || type === "")) {
      files.set(parts.join("/"), data);
      addToTree(dirs, parts, false);
    } else if (parts.length && type === "5") {
      addToTree(dirs, parts, true);
    }
    offset = dataStart + Math.ceil(size / 512) * 512;
  }
  return { files, dirs };
}
const normalizeAssetPath = (p) => p.split("/").filter(Boolean).join("/");
function makeAssetProvider(index) {
  return {
    stat(path) {
      const p = normalizeAssetPath(path);
      const file = index.files.get(p);
      if (file) return { size: file.byteLength, isDir: false };
      if (index.dirs.has(p)) return { size: 0, isDir: true };
      return null;
    },
    readdir(path) {
      const children = index.dirs.get(normalizeAssetPath(path));
      if (!children) throw new Error(`asset-fs: no such directory ${path}`);
      return [...children];
    },
    readFile(path) {
      const file = index.files.get(normalizeAssetPath(path));
      if (!file) throw new Error(`asset-fs: no such file ${path}`);
      return file;
    },
  };
}

/* ---- WebGPU: there has to be an adapter (a software one works, slowly) ---- */
async function probeGpu() {
  if (!navigator.gpu) {
    return { fatal: "This browser has no WebGPU, so Blender cannot start here." };
  }
  try {
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
      return { fatal: "No WebGPU adapter: turn on the browser's hardware acceleration and open Blender again." };
    }
    const info = adapter.info || {};
    const desc = [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(" | ");
    const software = adapter.isFallbackAdapter || /swiftshader|software|llvmpipe|lavapipe|warp|microsoft basic/i.test(desc);
    log("WEBGPU_ADAPTER " + (desc || "(no description)") + (software ? " (software)" : ""));
    return { desc, software };
  } catch (e) {
    return { fatal: "WebGPU failed: " + (e && e.message || e) };
  }
}

/* ---- the downloads: the engine (compiled as it streams in) and Blender's files, with one progress bar ---- */
const progress = { wasm: [0, 0], assets: [0, 0] };
const showProgress = () => {
  const loaded = progress.wasm[0] + progress.assets[0];
  const total = progress.wasm[1] + progress.assets[1];
  setStatus("Loading Blender: " + (loaded / 1048576).toFixed(0) + " of " + (total ? (total / 1048576).toFixed(0) : "?") + " MB",
            total ? loaded / total * 0.9 : 0);
};
// (The files may come compressed (Content-Encoding): Content-Length is then the packed size while the bytes read
// are the unpacked ones. release/manifest.json, fetch-release.sh's, has the unpacked sizes: "blender.wasm.zst": N.)
let decodedSizes = null;
const decodedSize = async (name) => {
  if (!decodedSizes) {
    try {
      const r = await fetch("release/manifest.json");
      decodedSizes = r.ok ? await r.json() : {};
    } catch (e) {
      decodedSizes = {};
    }
  }
  return Number(decodedSizes[name + ".zst"]) || 0;
};
const counted = (response, key, decoded) => {
  progress[key][1] = (response.headers.get("Content-Encoding") && decoded) || Number(response.headers.get("Content-Length")) || decoded || 0;
  const reader = response.body.getReader();
  const stream = new ReadableStream({
    async pull(controller) {
      const { done, value } = await reader.read();
      if (done) {
        controller.close();
        return;
      }
      progress[key][0] += value.byteLength;
      showProgress();
      controller.enqueue(value);
    },
  });
  return new Response(stream, { headers: response.headers });
};
async function fetchEngine() {
  const decoded = await decodedSize("blender.wasm");
  const r = await fetch("release/blender.wasm");
  if (!r.ok) throw new Error("release/blender.wasm: HTTP " + r.status);
  return WebAssembly.compileStreaming(counted(r, "wasm", decoded));
}
async function fetchAssets() {
  const decoded = await decodedSize("assets.tar");
  const r = await fetch("release/assets.tar");
  if (!r.ok) throw new Error("release/assets.tar: HTTP " + r.status);
  return new Uint8Array(await counted(r, "assets", decoded).arrayBuffer());
}

/* ---- the shader translation cache (IndexedDB of this origin, seeded with the release's) ---- */
const idbOpen = () => new Promise((res, rej) => {
  const rq = indexedDB.open("blender-fs", 2);
  rq.onupgradeneeded = () => {
    const db = rq.result;
    if (!db.objectStoreNames.contains("files")) db.createObjectStore("files", { keyPath: "path" });
    if (!db.objectStoreNames.contains("wgsl")) db.createObjectStore("wgsl", { keyPath: "key" });
  };
  rq.onsuccess = () => res(rq.result);
  rq.onerror = () => rej(rq.error);
});

/* ---- input: keys and the wheel stay with Blender (Puter's handlers: under PROXY_TO_PTHREAD emscripten cannot
 * cancel the browser's default in time), except what belongs to the desktop ---- */
const eatGesture = (e) => e.preventDefault();
canvas.addEventListener("wheel", eatGesture, { passive: false });
canvas.addEventListener("gesturestart", eatGesture);
canvas.addEventListener("gesturechange", eatGesture);
canvas.addEventListener("gestureend", eatGesture);
canvas.addEventListener("mousedown", (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener("dragstart", eatGesture);
window.addEventListener("contextmenu", (e) => e.preventDefault());
const isEditableTarget = (el) => !!el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.tagName === "SELECT" || el.isContentEditable);
const NAV_KEYS = new Set(["Tab", " ", "Spacebar", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "PageUp", "PageDown",
                          "Home", "End", "Backspace", "/"]);
window.addEventListener("keydown", (e) => {
  // The desktop's: Alt+Tab (and Alt+`) switch windows, Meta alone opens the launcher. The page takes the
  // keyboard for them; Blender does not see these keys.
  if (e.altKey && (e.code === "Tab" || e.code === "Backquote")) {
    e.preventDefault();
    e.stopImmediatePropagation();
    switching = true;
    toParent({ type: "switch", direction: e.shiftKey ? -1 : 1, alt: e.getModifierState("AltGraph") ? "AltRight" : "AltLeft" });
    return;
  }
  if (askEl.classList.contains("shown")) {
    if (e.code === "Escape") {
      document.getElementById("ask-no").click();
    }
    return;
  }
  if (!window.Module) return;
  if (isEditableTarget(document.activeElement)) return;
  if (e.ctrlKey || e.metaKey || NAV_KEYS.has(e.key)) e.preventDefault();
}, { capture: true });
// Alt let go here before the page had the keyboard (a quick Alt+Tab): the page picks as if it had seen it.
let switching = false;
window.addEventListener("keyup", (e) => {
  if (switching && (e.code === "AltLeft" || e.code === "AltRight")) {
    switching = false;
    e.preventDefault();
    e.stopImmediatePropagation();
    toParent({ type: "switch-end" });
  }
}, { capture: true });
window.addEventListener("blur", () => {
  switching = false;
});
// A click anywhere here brings the window forward (the page does not see clicks in this frame).
window.addEventListener("pointerdown", () => toParent({ type: "activate" }), { capture: true });
window.addEventListener("focus", () => toParent({ type: "activate" }));

/* ---- the canvas fills the frame (the window's size, which the desktop sets) ---- */
const fitCanvas = () => {
  const dpr = 1;	// (Blender's web build sizes its UI by the canvas's pixels; one canvas pixel per CSS pixel)
  const w = Math.max(1, Math.round(window.innerWidth * dpr)), h = Math.max(1, Math.round(window.innerHeight * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
};
fitCanvas();
window.addEventListener("resize", fitCanvas);

/* ---- Blender's title (the file's name) to the window ---- */
let lastTitle = "";
const sendTitle = () => {
  const t = document.title || "Blender";
  if (t !== lastTitle) {
    lastTitle = t;
    toParent({ type: "title", title: t.slice(0, 200) });
  }
};
new MutationObserver(sendTitle).observe(document.querySelector("title"), { childList: true, characterData: true, subtree: true });

/* ---- the start ---- */
let blenderUp = false;
window.__BGUI__ = { booted: false, device: false, window: false };
const scan = (t) => {
  if (t.includes("WEBGPU_CONTEXT") && t.includes("real device acquired")) window.__BGUI__.device = true;
  if (t.includes("WGPU_FIRST_CONTENT") && !window.__BGUI__.window) {
    window.__BGUI__.window = true;
    blenderUp = true;
    splashEl.classList.add("done");
    canvas.classList.add("ready");
    canvas.focus({ preventScroll: true });
    toParent({ type: "ready" });
    sendTitle();
  }
};

async function start() {
  setStatus("Loading Blender…", 0);
  const gpuProbe = probeGpu();
  const [gpu, engine, assets, initNow] = await Promise.all([gpuProbe, fetchEngine(), fetchAssets(), initPromise]);
  if (gpu.fatal) {
    fail(gpu.fatal);
    return;
  }
  if (gpu.software) {
    console.warn("WebGPU is a software renderer here (" + gpu.desc + "): Blender will be slow.");
  }
  setStatus("Starting Blender…", 0.95);
  const assetProvider = makeAssetProvider(indexTar(assets));
  const home = initNow.home;
  const args = ["--factory-startup"];
  if (initNow.open) {
    args.push(initNow.open);
  }

  // File > Open and File > Save: Puter's hooks would pick a folder of this computer; here the machine's home
  // is already mounted, so Blender's own file browser opens there.
  window.__blenderFileOpenHook = () => {
    window.Module.ccall("blender_web_file_open_at", null, ["string"], [home + "/"]);
  };
  window.__blenderSaveHook = () => {
    window.Module.ccall("blender_web_file_save_at", null, ["string"], [home + "/"]);
  };
  // (Only for a browser without the File System Access API, which this build is told it has: a save written
  // to memory is put into the home directory instead of being downloaded.)
  window.__blenderSaveDownload = (path) => {
    try {
      const data = window.Module.FS.readFile(path);
      const name = path.split("/").pop() || "untitled.blend";
      homeProvider.writeFile(name, data).then(() => log("saved " + home + "/" + name), (e) => console.error("save: " + e.message));
      try { window.Module.FS.unlink(path); } catch (e) {}
    } catch (e) {
      console.error("save: " + (e && e.message || e));
    }
  };

  window.Module = {
    arguments: args,
    canvas,
    // (WGPU_RP: a line per render pass the build's WebGPU backend begins, every frame: not kept)
    print: (t) => { if (!t.startsWith("WGPU_RP")) { log(t); scan(t); } },
    printErr: (t) => { if (!t.startsWith("WGPU_RP")) { log("[err] " + t); scan(t); } },
    instantiateWasm: (imports, cb) => {
      WebAssembly.instantiate(engine, imports).then((instance) => cb(instance, engine), (e) => fail("Blender's engine did not start: " + e.message));
      return {};
    },
    preRun: [function () {
      try {
        ENV.BLENDER_WEB_OPFS = "1";
        ENV.HOME = "/opfs/home";
        ENV.BLENDER_SYSTEM_RESOURCES = "/assets/5.3";
        ENV.BLENDER_SYSTEM_PYTHON = "/assets";
        ENV.PYTHONHOME = "/assets";
      } catch (e) { log("preRun ENV: " + e); }
      Module.geckoProviders = { 0: assetProvider, [HOME_MOUNT]: homeProvider };
      ENV.BLENDER_WEB_ASSET_PROVIDER = "1";
      addRunDependency("wgsl-cache");
      (async () => {
        const db = await idbOpen();
        const rows = await new Promise((res, rej) => {
          const rq = db.transaction("wgsl").objectStore("wgsl").getAll();
          rq.onsuccess = () => res(rq.result); rq.onerror = () => rej(rq.error);
        });
        let seed = [];
        try {
          const rsp = await fetch("release/wgsl-cache.json");
          if (rsp.ok) seed = await rsp.json();
        } catch (e) {}
        globalThis.__WGSL_CACHE__ = new Map([...seed, ...rows.map((r) => [r.key, r.wgsl])]);
        globalThis.__WGSL_CACHE_PUT__ = (k, v) => {
          try { db.transaction("wgsl", "readwrite").objectStore("wgsl").put({ key: k, wgsl: v }); } catch (e) {}
        };
      })().catch((e) => log("wgsl cache: " + e)).finally(() => removeRunDependency("wgsl-cache"));
    }],
    onRuntimeInitialized: () => {
      // The machine's home at /root (before main() runs: a file named on the command line is read from it).
      if (home) try {
        const rc = window.Module.ccall("blender_web_mount_provider", "number", ["number", "string"], [HOME_MOUNT, home]);
        if (rc !== 0) throw new Error("mount failed (" + rc + ")");
        // (The backend makes its mount point read-only, 0555; the files under it may be written.)
        window.Module.FS.chmod(home, 0o777);
        log("the machine's home is at " + home);
      } catch (e) {
        console.error("mounting the machine's home: " + (e && e.message || e));
      }
      try { window.Module.FS.mkdir("/mnt"); } catch (e) {}
      try {
        window.Module.ccall("blender_web_set_has_fsaccess", null, ["number"], [1]);
      } catch (e) { log("set_has_fsaccess: " + e); }
    },
    onAbort: (w) => fail("Blender stopped: " + w),
  };
  const s = document.createElement("script");
  s.src = "release/blender.js";
  s.onerror = () => fail("release/blender.js did not load");
  document.body.appendChild(s);
}

start().catch((e) => fail("Blender could not start: " + (e && e.message || e)));
