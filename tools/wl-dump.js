// The window server's surface table, read out of the machine's memory under
// tools/node-machine.js (--hook tools/wl-dump.js), and every window's pixels
// as PNG files: the way to see what clients draw without a browser. The hook
// also does what the page does with the table (acknowledges what it "showed"),
// so that clients get their frame callbacks and buffer releases and go on
// drawing, and prints what each window's slot says every time it changes.
//
//   WL_DUMP_DIR=build/shots WL_DUMP_EVERY=500 node ... --hook tools/wl-dump.js
//
// WL_DUMP_DIR: where the PNGs go (default build/shots), as SLOT-PRESENT.png;
// WL_DUMP_EVERY: milliseconds between looks at the table (default 100);
// WL_DUMP_MAX: at most this many PNGs per slot (default 20; 0 for none);
// WL_DUMP_ACTIONS: what a person at the page would do, in order, separated by "|" (input goes into the
//   kernel's input ring the way the page's canvas puts it there, os.screen_input):
//     wait:S              do nothing for S seconds
//     windows:N[,S]       wait (up to S seconds, default 120) until N toplevels are mapped
//     title:TEXT[,S]      wait until a window's title has TEXT in it
//     settle:S            wait until no window has presented anything for S seconds (at most 60)
//     move:X,Y            the pointer to X,Y (screen pixels)
//     click:X,Y           left button there (rclick: the right one; mclick: the middle one; dclick: twice);
//                         down:X,Y up:X,Y
//     drag:X,Y,X2,Y2      press, move in steps, let go
//     slowdrag:X,Y,X2,Y2  the same as a person does it: a pause after the press, small steps first, 2 s in all
//     wheel:N             N clicks of the wheel (up positive)
//     type:TEXT           the keys that make TEXT on a US keyboard
//     key:NAME            one key (Enter, Escape, Tab, Backspace, Up, Down, Left, Right, F1..F12, a letter),
//                         with modifiers before it: key:ctrl+shift+c
//     latency:CHAR        press CHAR (a US key) and report the milliseconds until a window next presents (the echo), and until
//                         the screen has been quiet for 300 ms (the run of presents it caused)
//     shot:NAME           the composed screen (windows by their order, decorated ones in a plain frame) as NAME.png
//     clip                print the guest's clipboard as the page would give it to the host
//     hostclip:TEXT       put TEXT in the table as the page does with the host's clipboard, and wait for the server
//     order:OP,ID,A,B,C   an order as the page writes it (README: the orders ring); ID "top" is the topmost toplevel
//     table               print the windows the table has
//     desktops            print the virtual desktops as the table has them (PLASMA-PROTOCOL.md)
//     screen:W,H[,S[,P[,T,L,R]]]  (T, L, R: the panels' strips at the top, left and right)
//     screen:W,H[,S[,P]]  ask for a screen of W x H at scale S (default 1) with a panel of P pixels at the
//                         bottom, as the page does, and wait for it
//     apps                print the programs the launcher has (their numbers are what launch: takes)
//     launch:NAME         start the program whose name has NAME in it, as the page's launcher does
//     layout:NAME         ask for the keyboard layout NAME (wlserver/keymaps/index.txt), and wait for it
//     codes:C C+C ...     keys by their evdev codes: each item is pressed and let go, "+" holds the ones before
//                         it down (codes:100+18 is AltGr-E); "d42" / "u42" press or let go one only
//     drop:FILE[,X,Y]     a file from the host (this computer), as the page sends one that was dropped on it,
//                         at X,Y (or nowhere); waits for the server and prints where it went
"use strict";

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const DIR = process.env.WL_DUMP_DIR || "build/shots";
const EVERY = Number(process.env.WL_DUMP_EVERY) || 100;
const MAX = process.env.WL_DUMP_MAX === undefined ? 20 : Number(process.env.WL_DUMP_MAX);
const ACTIONS = process.env.WL_DUMP_ACTIONS || "";

// The layout (wlserver/README.md).
const H_MAGIC = 0, H_PID = 8, H_HEARTBEAT = 12, H_SLOTS = 16, H_SLOT_SIZE = 20, H_SCREEN_W = 24, H_SCREEN_H = 28,
      H_GENERATION = 32, H_PAGE_FRAME = 64, H_PAGE_TIME = 68;
const S_SEQ = 0, S_ID = 4, S_PARENT = 8, S_KIND = 12, S_X = 16, S_Y = 20, S_W = 24, S_H = 28, S_Z = 32, S_FLAGS = 36,
      S_GEOM = 40, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_STRIDE = 68, S_FORMAT = 72, S_BUF_W = 76,
      S_BUF_H = 80, S_SCALE = 84, S_DAMAGE_COUNT = 88, S_DAMAGE = 92, S_UPLOADED = 224, S_SHOWN = 228,
      S_TITLE = 256, S_APP_ID = 320, S_DESKTOP = 232, S_SHELL_FLAGS = 236, S_TILE = 240;
const H_DESKTOPS = 1928, H_DESKTOP_CURRENT = 1932, H_DESKTOP_ROWS = 1936, H_DESKTOPS_LO = 1940, H_DESKTOPS_HI = 1944,
      H_DESKTOPS_LEN = 1948, H_DESKTOPS_SERIAL = 1952, H_WORK = 1960;
const SH_SHADED = 4;
const H_TITLE_HEIGHT = 136, H_BORDER = 140, H_GUEST_CLIP_SERIAL = 144, H_GUEST_CLIP_LO = 148, H_GUEST_CLIP_HI = 152,
      H_GUEST_CLIP_LEN = 156, H_HOST_CLIP_LO = 160, H_HOST_CLIP_HI = 164, H_HOST_CLIP_CAP = 168,
      H_HOST_CLIP_SERIAL = 172, H_HOST_CLIP_LEN = 176, H_HOST_CLIP_ACK = 180, H_FOCUS = 184,
      H_CURSOR_SERIAL = 96, H_CURSOR_KIND = 100, H_CURSOR_SHAPE = 104,
      H_ORDER_HEAD = 256, H_ORDER_TAIL = 260, H_ORDERS = 264, ORDERS = 32, ORDER_SIZE = 32;
const H_OUT_SERIAL = 188, H_OUT_W = 192, H_OUT_H = 196, H_OUT_SCALE = 200, H_OUT_ACK = 204, H_SCREEN_SCALE = 208,
      H_KBD_SERIAL = 212, H_KBD_ACK = 216, H_KBD_STATUS = 220, H_KBD_NAME = 224, H_KBD_CURRENT = 1288,
      H_XFER_LO = 1320, H_XFER_HI = 1324, H_XFER_CAP = 1328, H_XFER_SERIAL = 1332, H_XFER_LEN = 1336,
      H_XFER_FLAGS = 1340, H_XFER_TOTAL = 1344, H_XFER_X = 1348, H_XFER_Y = 1352, H_XFER_ACK = 1356,
      H_XFER_STATUS = 1360, H_XFER_NAME = 1368, H_XFER_PATH = 1624;
const MAGIC = 0x4c445652;
const F_MINIMIZED = 64, F_DECORATED = 32, F_ACTIVATED = 2;

// evdev's numbers for keys (linux/input-event-codes.h), and the characters of a US keyboard.
const KEY = {
  esc: 1, escape: 1, backspace: 14, tab: 15, enter: 28, ctrl: 29, control: 29, shift: 42, alt: 56, space: 57,
  capslock: 58, meta: 125, super: 125, up: 103, left: 105, right: 106, down: 108, home: 102, end: 107, pageup: 104,
  pagedown: 109, insert: 110, delete: 111, menu: 127,
};
for (let i = 1; i <= 10; i++) {
  KEY["f" + i] = 58 + i;
}
KEY.f11 = 87;
KEY.f12 = 88;
const CHARS = {};
"1234567890-=".split("").forEach((c, i) => {
  CHARS[c] = [2 + i, false];
});
"!@#$%^&*()_+".split("").forEach((c, i) => {
  CHARS[c] = [2 + i, true];
});
[["qwertyuiop[]", 16], ["asdfghjkl;'`", 30], ["zxcvbnm,./", 44]].forEach(([row, first]) => {
  row.split("").forEach((c, i) => {
    CHARS[c] = [first + i, false];
    if (/[a-z]/.test(c)) {
      CHARS[c.toUpperCase()] = [first + i, true];
    }
  });
});
Object.assign(CHARS, { "{": [26, true], "}": [27, true], ":": [39, true], "\"": [40, true], "~": [41, true],
                       "\\": [43, false], "|": [43, true], "<": [51, true], ">": [52, true], "?": [53, true],
                       " ": [57, false], "\n": [28, false], "\t": [15, false] });
for (const c of "abcdefghijklmnopqrstuvwxyz") {
  KEY[c] = CHARS[c][0];
}
for (const c of "0123456789") {
  KEY[c] = CHARS[c][0];
}
Object.assign(KEY, { minus: 12, equal: 13, comma: 51, period: 52, slash: 53 });

// A PNG of BGRA/BGRX pixels (little-endian ARGB8888/XRGB8888 words) at `address` in `memory`.
// format: the slot's (0 ARGB8888, 1 XRGB8888, 3 ABGR8888, 4 XBGR8888: bytes B G R A, or R G B A for 3 and 4).
const png = (memory, address, stride, w, h, format) => {
  const xrgb = format === 1 || format === 4, rgba = format === 3 || format === 4;
  const src = new Uint8Array(memory.buffer, address, stride * h);
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    const row = y * (w * 4 + 1);
    raw[row] = 0;	// filter: none
    for (let x = 0; x < w; x++) {
      const s = y * stride + x * 4, d = row + 1 + x * 4;
      raw[d] = src[rgba ? s : s + 2];
      raw[d + 1] = src[s + 1];
      raw[d + 2] = src[rgba ? s + 2 : s];
      raw[d + 3] = xrgb ? 255 : src[s + 3];
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;	// bit depth
  ihdr[9] = 6;	// RGBA
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
                        chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
};
const pngRGBA = (rgba, w, h) => {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
                        chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
};
const table = new Int32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) {
    c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  }
  table[n] = c;
}
const crc32 = (buf) => {
  let c = -1;
  for (let i = 0; i < buf.length; i++) {
    c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ -1) >>> 0;
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The composed screen: every slot by its order, decorated windows in a plain frame (the page draws a better one).
// What the page has of each surface: its texture, which the page updates with the damaged rectangles when it
// uploads a present (display.js), and draws from afterwards. Kept here the same way (by surface id, the
// buffer's pixels as B, G, R, A), so that a composed shot shows what the page would, not what is in a buffer that
// the client has been given back and may be drawing into again.
const textures = new Map();
const upload = (memory, s) => {
  let t = textures.get(s.id);
  if (!t || t.bw !== s.bw || t.bh !== s.bh) {
    t = { bw: s.bw, bh: s.bh, pixels: new Uint8Array(s.bw * s.bh * 4) };
    textures.set(s.id, t);
    s = Object.assign({}, s, { damageCount: 0xffffffff });
  }
  const src = new Uint8Array(memory.buffer);
  const rects = s.damageCount === 0xffffffff ? [[0, 0, s.bw, s.bh]] : s.damage;
  for (const r of rects) {
    const x0 = Math.max(0, r[0]), y0 = Math.max(0, r[1]);
    const x1 = Math.min(s.bw, r[0] + r[2]), y1 = Math.min(s.bh, r[1] + r[3]);
    for (let y = y0; y < y1; y++) {
      const from = s.buffer + y * s.stride + x0 * 4;
      t.pixels.set(src.subarray(from, from + (x1 - x0) * 4), (y * s.bw + x0) * 4);
    }
  }
};

const compose = (memory, slots, width, height, chromeTitle, chromeBorder, desktop = 0) => {
  const out = new Uint8Array(width * height * 4);
  for (let i = 0; i < out.length; i += 4) {
    out[i] = 0x1d;
    out[i + 1] = 0x2b;
    out[i + 2] = 0x36;
    out[i + 3] = 255;
  }
  const fill = (x0, y0, w, h, r, g, b) => {
    for (let y = Math.max(0, y0); y < Math.min(height, y0 + h); y++) {
      for (let x = Math.max(0, x0); x < Math.min(width, x0 + w); x++) {
        const d = (y * width + x) * 4;
        out[d] = r;
        out[d + 1] = g;
        out[d + 2] = b;
      }
    }
  };
  const byId = new Map(slots.map((s) => [s.id, s]));
  for (const s of slots.slice().sort((a, b) => a.z - b.z)) {
    let x = s.x, y = s.y;
    if (s.kind === 3) {
      for (let p = byId.get(s.parent); p; p = p.kind === 3 ? byId.get(p.parent) : null) {
        x += p.x;
        y += p.y;
      }
    }
    let hidden = false, shaded = false;
    for (let p = s; p; p = p.parent ? byId.get(p.parent) : null) {
      if (p.flags & F_MINIMIZED) {
        hidden = true;
      }
      if (!p.parent) {
        // (a window on another desktop than the current one is not drawn; a shaded one only as its title bar)
        if (desktop && p.desktop && p.desktop !== desktop) {
          hidden = true;
        }
        shaded = !!(p.shellFlags & SH_SHADED);
        break;
      }
    }
    const tex = textures.get(s.id);
    if (hidden || !s.buffer || !s.bw || s.format === 2 || !tex || tex.bw !== s.bw || tex.bh !== s.bh) {	// (format 2: a GPU resource, not pixels here)
      continue;
    }
    if (s.flags & F_DECORATED) {
      const g = s.geometry;
      const gx = x + g[0], gy = y + g[1];
      const lit = s.flags & F_ACTIVATED;
      fill(gx - chromeBorder, gy - chromeTitle - chromeBorder, g[2] + 2 * chromeBorder,
           (shaded ? 0 : g[3]) + chromeTitle + 2 * chromeBorder, lit ? 0x3d : 0x55, lit ? 0x6e : 0x5c, lit ? 0x9a : 0x66);
    }
    if (shaded) {
      continue;
    }
    const sw = Math.min(s.w, s.bw), sh = Math.min(s.h, s.bh);
    for (let row = 0; row < sh; row++) {
      const yy = y + row;
      if (yy < 0 || yy >= height) {
        continue;
      }
      for (let col = 0; col < sw; col++) {
        const xx = x + col;
        if (xx < 0 || xx >= width) {
          continue;
        }
        const src = tex.pixels;
        const a = (row * s.bw + col) * 4;
        const d = (yy * width + xx) * 4;
        const alpha = s.format === 1 || s.format === 4 ? 255 : src[a + 3];
        const k = 255 - alpha;
        const rgba = s.format === 3 || s.format === 4;
        out[d] = src[rgba ? a : a + 2] + ((out[d] * k) / 255 | 0);
        out[d + 1] = src[a + 1] + ((out[d + 1] * k) / 255 | 0);
        out[d + 2] = src[rgba ? a + 2 : a] + ((out[d + 2] * k) / 255 | 0);
      }
    }
  }
  return out;
};

module.exports = (os, run) => {
  fs.mkdirSync(DIR, { recursive: true });
  let announced = null;
  const channel = new BroadcastChannel("rv-display");
  channel.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "table" && (!announced || announced.address !== m.address)) {
      announced = m;
      console.error("[wl-dump] table at " + m.address + " (pid " + m.pid + ", " + m.slots + " slots of " + m.slotSize + ")");
    }
  };
  // What the server tells the page on rv-plasma beyond the configuration: notifications, tray items.
  const plasmaChannel = new BroadcastChannel("rv-plasma");
  plasmaChannel.onmessage = (event) => {
    const m = event.data;
    if (m && (m.type === "notify" || m.type === "notify-close" || m.type === "tray")) {
      console.error("[wl-dump] rv-plasma " + JSON.stringify(m, (k, x) => (x instanceof Uint8Array ? "<" + x.length + " bytes>" : x)).slice(0, 400));
    }
  };
  const seen = new Map();	// slot -> { present, count }
  let current = [];		// the slots as last read
  let lastPresentAt = Date.now();
  let frame = 0, dumped = 0, lastBeat = -1, beats = 0;
  const look = () => {
    if (!announced || !os.memory) {
      return;
    }
    const memory = os.memory;
    const v = new DataView(memory.buffer);
    const w32 = new Int32Array(memory.buffer);
    const t = announced.address;
    let bell = false;
    if (v.getUint32(t + H_MAGIC, true) !== MAGIC) {
      console.error("[wl-dump] the table's magic is gone (the server ended?)");
      announced = null;
      return;
    }
    const beat = v.getUint32(t + H_HEARTBEAT, true);
    if (beat !== lastBeat) {
      beats++;
      lastBeat = beat;
    }
    frame++;
    Atomics.store(w32, (t + H_PAGE_FRAME) / 4, frame);
    Atomics.store(w32, (t + H_PAGE_TIME) / 4, Date.now() & 0x7fffffff);
    const slots = v.getUint32(t + H_SLOTS, true), slotSize = v.getUint32(t + H_SLOT_SIZE, true);
    const base = t + announced.header;
    current = [];
    for (let i = 0; i < slots; i++) {
      const at = base + i * slotSize;
      let seq1, seq2, s, tries = 0, since = 0;
      do {
        // (A slot that never settles for a second: the server ended and its memory went to someone else, with
        // the magic still in place. Without this the look spun here for good, the main thread with it. By time,
        // not by count: on a busy machine a live writer can be held off in the middle of a slot for a while.)
        if ((++tries & 1023) === 0 && (since = since || Date.now()) && Date.now() - since > 1000) {
          console.error("[wl-dump] slot " + i + " never settles (the server ended?): the table is let go");
          announced = null;
          return;
        }
        seq1 = Atomics.load(w32, at / 4);
        if (seq1 & 1) {
          continue;
        }
        s = {
          id: v.getUint32(at + S_ID, true), parent: v.getUint32(at + S_PARENT, true), kind: v.getUint32(at + S_KIND, true),
          x: v.getInt32(at + S_X, true), y: v.getInt32(at + S_Y, true), w: v.getUint32(at + S_W, true), h: v.getUint32(at + S_H, true),
          z: v.getUint32(at + S_Z, true), flags: v.getUint32(at + S_FLAGS, true),
          geometry: [0, 1, 2, 3].map((k) => v.getInt32(at + S_GEOM + 4 * k, true)),
          present: v.getUint32(at + S_PRESENT, true),
          buffer: v.getUint32(at + S_BUFFER_LO, true) + v.getUint32(at + S_BUFFER_HI, true) * 0x100000000,
          stride: v.getUint32(at + S_STRIDE, true), format: v.getUint32(at + S_FORMAT, true),
          gpuSerial: v.getUint32(at + 244, true),
          bw: v.getUint32(at + S_BUF_W, true), bh: v.getUint32(at + S_BUF_H, true), scale: v.getUint32(at + S_SCALE, true),
          damageCount: v.getUint32(at + S_DAMAGE_COUNT, true), damage: [],
          title: text(memory, at + S_TITLE), appId: text(memory, at + S_APP_ID),
          desktop: v.getUint32(at + S_DESKTOP, true), shellFlags: v.getUint32(at + S_SHELL_FLAGS, true),
          tile: v.getUint32(at + S_TILE, true),
        };
        if (s.damageCount !== 0xffffffff) {
          for (let d = 0; d < Math.min(s.damageCount, 8); d++) {
            s.damage.push([0, 1, 2, 3].map((k) => v.getInt32(at + S_DAMAGE + 16 * d + 4 * k, true)));
          }
        }
        seq2 = Atomics.load(w32, at / 4);
      } while (seq1 !== seq2 || (seq1 & 1));
      // (A slot of a table whose server ended and whose memory was reused, magic still in place, can settle with garbage in it: a
      // buffer address beyond the memory. The server of `desktop wayland sh -c ...` that ends with its first client leaves such a table.
      // Let it go: the live server announces its own every second.)
      if (s.id && s.format !== 2 && s.buffer && s.buffer + s.stride * s.bh > memory.buffer.byteLength) {
        console.error("[wl-dump] slot " + i + " has a buffer beyond the memory (a table that outlived its server): the table is let go");
        announced = null;
        return;
      }
      if (s.id) {
        current.push(s);
      }
      if (!s.id) {
        if (seen.has(i)) {
          console.error("[wl-dump] slot " + i + " freed");
          seen.delete(i);
        }
        continue;
      }
      const was = seen.get(i);
      if (!was || was.present !== s.present || was.id !== s.id) {
        // (a buffer size past 16384 is a slot read while it was being remade: nothing to upload from it)
        if (s.buffer && s.bw && s.bh && s.bw <= 16384 && s.bh <= 16384 && s.format !== 2) {
          upload(memory, s);
        }
        const count = was ? was.count + 1 : 1;
        seen.set(i, { present: s.present, count, id: s.id });
        lastPresentAt = Date.now();
        const bytes = s.damageCount === 0xffffffff ? s.stride * s.bh : s.damage.reduce((n, r) => n + r[2] * r[3] * 4, 0);
        console.error("[wl-dump] slot " + i + " id " + s.id + " kind " + s.kind + " at " + s.x + "," + s.y + " " + s.w + "x" + s.h
                      + " z " + s.z + " flags " + s.flags + " present " + s.present + " buffer " + s.buffer + " stride " + s.stride
                      + " format " + s.format + (s.format === 2 ? " (GPU resource " + (s.buffer % 0x100000000) + " of the GPU server "
                      + Math.floor(s.buffer / 0x100000000) + ", gpu_serial " + s.gpuSerial + ")" : "") + " damage " + (s.damageCount === 0xffffffff ? "all" : JSON.stringify(s.damage))
                      + " (" + bytes + " bytes) title " + JSON.stringify(s.title) + " app " + JSON.stringify(s.appId)
                      + " desk " + s.desktop + " sf " + s.shellFlags + " tile " + s.tile);
        if (count <= MAX && MAX > 0 && s.buffer && s.bw && s.bh && s.format !== 2) {
          const file = path.join(DIR, i + "-" + s.present + ".png");
          fs.writeFileSync(file, png(memory, s.buffer, s.stride, s.bw, s.bh, s.format));
          dumped++;
        }
      }
      // What the page would say: uploaded and shown, this present (and its doorbell, as display.js rings it).
      Atomics.store(w32, (at + S_UPLOADED) / 4, s.present);
      Atomics.store(w32, (at + S_SHOWN) / 4, s.present);
      bell = true;
    }
    if (bell) {
      os.screen_input(5, 1, 0);
    }
  };
  const timer = setInterval(look, EVERY);

  // ---- the actions
  const SCREEN_W = () => announced && os.memory ? header().getUint32(announced.address + H_SCREEN_W, true) : 1024;
  const SCREEN_H = () => announced && os.memory ? header().getUint32(announced.address + H_SCREEN_H, true) : 768;
  const putText = (at, value, size) => {
    const m = new Uint8Array(os.memory.buffer);
    m.fill(0, at, at + size);
    m.set(new TextEncoder().encode(value).subarray(0, size - 1), at);
  };
  let ptr = [512, 384];
  const position = (x, y) => {
    ptr = [x, y];
    os.screen_input(1, Math.round(x / SCREEN_W() * 32767), Math.round(y / SCREEN_H() * 32767));
  };
  const keyTap = async (code, shift) => {
    if (shift) {
      os.screen_input(0, 42, 1);
    }
    os.screen_input(0, code, 1);
    await sleep(30);
    os.screen_input(0, code, 0);
    if (shift) {
      os.screen_input(0, 42, 0);
    }
    await sleep(30);
  };
  const toplevels = () => current.filter((s) => s.kind === 1 && !(s.flags & F_MINIMIZED));
  const waitFor = async (test, seconds, what) => {
    const until = Date.now() + seconds * 1000;
    while (Date.now() < until) {
      look();
      if (test()) {
        return true;
      }
      await sleep(200);
    }
    console.error("[wl-dump] gave up waiting for " + what);
    return false;
  };
  const header = () => new DataView(os.memory.buffer);
  const runActions = async () => {
    for (const action of ACTIONS.split("|").filter((a) => a.length)) {
      const colon = action.indexOf(":");
      const name = colon < 0 ? action : action.slice(0, colon);
      const arg = colon < 0 ? "" : action.slice(colon + 1);
      const nums = arg.split(",").map(Number);
      const t0 = Date.now();
      switch (name) {
      case "wait":
        await sleep(nums[0] * 1000);
        break;
      case "windows":
        await waitFor(() => toplevels().length >= nums[0], nums[1] || 120, nums[0] + " windows");
        break;
      case "title": {
        const parts = arg.split(",");
        await waitFor(() => current.some((s) => s.title.includes(parts[0])), Number(parts[1]) || 120, "title " + parts[0]);
        break;
      }
      case "settle":
        await waitFor(() => Date.now() - lastPresentAt > nums[0] * 1000, 60, "a quiet screen");
        break;
      case "move":
        position(nums[0], nums[1]);
        await sleep(50);
        break;
      case "click": case "rclick": case "mclick": {
        const button = { click: 0x110, rclick: 0x111, mclick: 0x112 }[name];
        position(nums[0], nums[1]);
        await sleep(50);
        os.screen_input(2, button, 1);
        await sleep(60);
        os.screen_input(2, button, 0);
        await sleep(50);
        break;
      }
      case "dclick":
        position(nums[0], nums[1]);
        await sleep(50);
        for (let k = 0; k < 2; k++) {
          os.screen_input(2, 0x110, 1);
          await sleep(40);
          os.screen_input(2, 0x110, 0);
          await sleep(60);
        }
        break;
      case "down": case "up":
        position(nums[0], nums[1]);
        await sleep(30);
        os.screen_input(2, 0x110, name === "down" ? 1 : 0);
        await sleep(30);
        break;
      case "drag": {
        position(nums[0], nums[1]);
        await sleep(50);
        os.screen_input(2, 0x110, 1);
        await sleep(80);
        for (let k = 1; k <= 10; k++) {
          position(nums[0] + (nums[2] - nums[0]) * k / 10, nums[1] + (nums[3] - nums[1]) * k / 10);
          await sleep(40);
        }
        await sleep(80);
        os.screen_input(2, 0x110, 0);
        await sleep(50);
        break;
      }
      case "slowdrag": {
        position(nums[0], nums[1]);
        await sleep(100);
        os.screen_input(2, 0x110, 1);
        await sleep(400);
        for (let k = 1; k <= 40; k++) {
          const f = k <= 5 ? k * 0.005 : 0.025 + (k - 5) / 35 * 0.975;
          position(nums[0] + (nums[2] - nums[0]) * f, nums[1] + (nums[3] - nums[1]) * f);
          await sleep(50);
        }
        await sleep(600);
        os.screen_input(2, 0x110, 0);
        await sleep(50);
        break;
      }
      case "wheel":
        os.screen_input(3, 0, nums[0]);
        await sleep(50);
        break;
      case "type":
        for (const c of arg.replace(/\\n/g, "\n")) {
          const k = CHARS[c];
          if (k) {
            await keyTap(k[0], k[1]);
          }
        }
        break;
      case "latency": {
        const k = CHARS[arg[0]];
        look();
        const before = lastPresentAt;
        const start = Date.now();
        os.screen_input(0, k[0], 1);
        await sleep(20);
        os.screen_input(0, k[0], 0);
        let first = 0;
        for (let spent = 0; spent < 10000; spent += 5) {
          await sleep(5);
          look();
          if (!first && lastPresentAt > before) {
            first = lastPresentAt - start;
          }
          if (first && Date.now() - lastPresentAt > 300) {
            break;
          }
        }
        console.log("[wl-dump] latency:" + arg + " first present " + first + " ms, last " + (lastPresentAt - start) + " ms");
        break;
      }
      case "key": {
        const parts = arg.toLowerCase().split("+");
        const main = parts.pop();
        const mods = parts.map((m) => KEY[m]).filter(Boolean);
        for (const m of mods) {
          os.screen_input(0, m, 1);
        }
        await sleep(30);
        await keyTap(KEY[main] || (CHARS[main] && CHARS[main][0]) || 0, false);
        for (const m of mods.reverse()) {
          os.screen_input(0, m, 0);
        }
        await sleep(30);
        break;
      }
      case "shot": {
        look();
        const v = header();
        const w = SCREEN_W(), h = SCREEN_H();
        const pixels = compose(os.memory, current, w, h, v.getUint32(announced.address + H_TITLE_HEIGHT, true),
                               v.getUint32(announced.address + H_BORDER, true), v.getUint32(announced.address + H_DESKTOP_CURRENT, true));
        const file = path.join(DIR, arg + ".png");
        fs.writeFileSync(file, pngRGBA(pixels, w, h));
        console.error("[wl-dump] " + file + ": " + current.map((s) => s.id + "/" + s.kind + "@" + s.x + "," + s.y + " "
                      + s.w + "x" + s.h + " z" + s.z + " f" + s.flags).join("; "));
        break;
      }
      case "clip": {
        const v = header(), a = announced.address;
        const serial = v.getUint32(a + H_GUEST_CLIP_SERIAL, true);
        const at = v.getUint32(a + H_GUEST_CLIP_LO, true) + v.getUint32(a + H_GUEST_CLIP_HI, true) * 0x100000000;
        const len = v.getUint32(a + H_GUEST_CLIP_LEN, true);
        const text = serial ? new TextDecoder().decode(new Uint8Array(os.memory.buffer, at, len).slice()) : null;
        console.error("[wl-dump] guest clipboard (serial " + serial + "): " + JSON.stringify(text));
        break;
      }
      case "hostclip": {
        const v = header(), a = announced.address;
        const at = v.getUint32(a + H_HOST_CLIP_LO, true) + v.getUint32(a + H_HOST_CLIP_HI, true) * 0x100000000;
        const raw = new TextEncoder().encode(arg);
        new Uint8Array(os.memory.buffer).set(raw, at);
        const w32 = new Int32Array(os.memory.buffer);
        const serial = (v.getUint32(a + H_HOST_CLIP_SERIAL, true) + 1) >>> 0;
        Atomics.store(w32, (a + H_HOST_CLIP_LEN) / 4, raw.length);
        Atomics.store(w32, (a + H_HOST_CLIP_SERIAL) / 4, serial);
        os.screen_input(5, 1, 0);	// (the doorbell)
        await waitFor(() => header().getUint32(a + H_HOST_CLIP_ACK, true) === serial, 5, "the server to take the host's text");
        break;
      }
      case "order": {
        const parts = arg.split(",");
        let id = Number(parts[1]);
        if (parts[1] === "top") {
          look();
          const tops = toplevels().sort((p, q) => q.z - p.z);
          id = tops.length ? tops[0].id : 0;
        }
        const v = header(), a = announced.address;
        const w32 = new Int32Array(os.memory.buffer);
        const head = v.getUint32(a + H_ORDER_HEAD, true);
        const at = a + H_ORDERS + (head % ORDERS) * ORDER_SIZE;
        const words = [Number(parts[0]), id, Number(parts[2] || 0), Number(parts[3] || 0), Number(parts[4] || 0),
                       Number(parts[5] || 0), 0, 0];
        words.forEach((x, k) => v.setInt32(at + 4 * k, x, true));
        Atomics.store(w32, (a + H_ORDER_HEAD) / 4, (head + 1) | 0);
        os.screen_input(5, 1, 0);	// (the doorbell)
        await sleep(50);
        break;
      }
      case "screen": {
        const v = header(), a = announced.address, w32 = new Int32Array(os.memory.buffer);
        v.setUint32(a + H_OUT_W, nums[0], true);
        v.setUint32(a + H_OUT_H, nums[1], true);
        v.setUint32(a + H_OUT_SCALE, nums[2] || 1, true);
        v.setUint32(a + 1884, nums[3] || 0, true);
        v.setUint32(a + 1916, nums[4] || 0, true);	// (the struts at the top, left and right: PLASMA-PROTOCOL.md)
        v.setUint32(a + 1920, nums[5] || 0, true);
        v.setUint32(a + 1924, nums[6] || 0, true);
        const serial = (v.getUint32(a + H_OUT_SERIAL, true) + 1) >>> 0;
        Atomics.store(w32, (a + H_OUT_SERIAL) / 4, serial);
        os.screen_input(5, 1, 0);	// (the doorbell)
        await waitFor(() => header().getUint32(a + H_OUT_ACK, true) === serial, 5, "the server to take the screen");
        console.error("[wl-dump] screen " + SCREEN_W() + "x" + SCREEN_H() + " scale " + header().getUint32(a + H_SCREEN_SCALE, true));
        break;
      }
      case "layout": {
        const v = header(), a = announced.address, w32 = new Int32Array(os.memory.buffer);
        putText(a + H_KBD_NAME, arg, 32);
        const serial = (v.getUint32(a + H_KBD_SERIAL, true) + 1) >>> 0;
        Atomics.store(w32, (a + H_KBD_SERIAL) / 4, serial);
        os.screen_input(5, 1, 0);	// (the doorbell)
        await waitFor(() => header().getUint32(a + H_KBD_ACK, true) === serial, 5, "the server to take the layout");
        console.error("[wl-dump] layout status " + header().getUint32(a + H_KBD_STATUS, true) + ", now "
                      + text(os.memory, a + H_KBD_CURRENT).slice(0, 32));
        break;
      }
      case "codes":
        for (const item of arg.split(/\s+/).filter(Boolean)) {
          if (/^[du]\d+$/.test(item)) {
            os.screen_input(0, Number(item.slice(1)), item[0] === "d" ? 1 : 0);
            await sleep(40);
            continue;
          }
          const list = item.split("+").map(Number);
          for (const c of list) {
            os.screen_input(0, c, 1);
            await sleep(30);
          }
          for (const c of list.reverse()) {
            os.screen_input(0, c, 0);
            await sleep(30);
          }
          await sleep(30);
        }
        break;
      case "drop": {
        const parts = arg.split(",");
        const data = fs.readFileSync(parts[0]);
        const v = header(), a = announced.address, w32 = new Int32Array(os.memory.buffer);
        const at = v.getUint32(a + H_XFER_LO, true) + v.getUint32(a + H_XFER_HI, true) * 0x100000000;
        const cap = v.getUint32(a + H_XFER_CAP, true);
        let sent = 0;
        do {
          const n = Math.min(cap, data.length - sent);
          new Uint8Array(os.memory.buffer).set(data.subarray(sent, sent + n), at);
          putText(a + H_XFER_NAME, path.basename(parts[0]), 256);
          v.setUint32(a + H_XFER_LEN, n, true);
          v.setUint32(a + H_XFER_TOTAL, data.length, true);
          v.setUint32(a + H_XFER_FLAGS, (sent === 0 ? 1 : 0) | (sent + n >= data.length ? 2 | 4 : 0), true);
          v.setInt32(a + H_XFER_X, parts.length > 2 ? Number(parts[1]) : -1, true);
          v.setInt32(a + H_XFER_Y, parts.length > 2 ? Number(parts[2]) : -1, true);
          const serial = (v.getUint32(a + H_XFER_SERIAL, true) + 1) >>> 0;
          Atomics.store(w32, (a + H_XFER_SERIAL) / 4, serial);
          os.screen_input(5, 1, 0);	// (the doorbell)
          await waitFor(() => header().getUint32(a + H_XFER_ACK, true) === serial, 30, "the server to take a piece");
          sent += n;
        } while (sent < data.length);
        console.error("[wl-dump] dropped " + data.length + " bytes: status " + header().getUint32(a + H_XFER_STATUS, true)
                      + ", " + text(os.memory, a + H_XFER_PATH, 256));
        break;
      }
      case "apps": case "launch": {
        const v = header(), a = announced.address;
        const at = v.getUint32(a + 1888, true) + v.getUint32(a + 1892, true) * 0x100000000;
        const list = JSON.parse(new TextDecoder().decode(new Uint8Array(os.memory.buffer, at, v.getUint32(a + 1896, true)).slice()));
        if (name === "apps") {
          console.error("[wl-dump] apps (" + list.length + "): " + list.map((x, i) => i + " " + x.name).join("; "));
          break;
        }
        const index = list.findIndex((x) => x.name.toLowerCase().includes(arg.toLowerCase()));
        const w32 = new Int32Array(os.memory.buffer);
        const head = v.getUint32(a + H_ORDER_HEAD, true);
        const o = a + H_ORDERS + (head % ORDERS) * ORDER_SIZE;
        [10, 0, index, v.getUint32(a + 1900, true), 0, 0, 0, 0].forEach((x, k) => v.setInt32(o + 4 * k, x, true));
        Atomics.store(w32, (a + H_ORDER_HEAD) / 4, (head + 1) | 0);
        os.screen_input(5, 1, 0);	// (the doorbell)
        await waitFor(() => header().getUint32(a + 1904, true) === ((head + 1) >>> 0), 10, "the launch");
        console.error("[wl-dump] launch " + index + " (" + (list[index] || {}).name + "): status " + header().getUint32(a + 1908, true));
        break;
      }
      case "desktops": {
        const v = header(), a = announced.address;
        const at = v.getUint32(a + H_DESKTOPS_LO, true) + v.getUint32(a + H_DESKTOPS_HI, true) * 0x100000000;
        const json = at ? new TextDecoder().decode(new Uint8Array(os.memory.buffer, at, v.getUint32(a + H_DESKTOPS_LEN, true)).slice()) : "";
        console.error("[wl-dump] desktops: " + v.getUint32(a + H_DESKTOPS, true) + ", current " + v.getUint32(a + H_DESKTOP_CURRENT, true)
                      + ", rows " + v.getUint32(a + H_DESKTOP_ROWS, true) + ", serial " + v.getUint32(a + H_DESKTOPS_SERIAL, true)
                      + ", work area " + [0, 1, 2, 3].map((k) => v.getInt32(a + H_WORK + 4 * k, true)).join(",") + "; " + json);
        break;
      }
      case "table":
        look();
        console.error("[wl-dump] table: focus " + header().getUint32(announced.address + H_FOCUS, true) + ", cursor kind "
                      + header().getUint32(announced.address + H_CURSOR_KIND, true) + " shape "
                      + header().getUint32(announced.address + H_CURSOR_SHAPE, true) + "; "
                      + current.map((s) => s.id + "/" + s.kind + " " + JSON.stringify(s.title) + " @" + s.x + "," + s.y + " "
                      + s.w + "x" + s.h + " geom " + s.geometry.join(",") + " z" + s.z + " f" + s.flags + " d" + s.desktop
                      + " sf" + s.shellFlags + " t" + s.tile).join("; "));
        break;
      default:
        console.error("[wl-dump] unknown action " + name);
      }
      console.error("[wl-dump] " + action.slice(0, 60) + " (" + ((Date.now() - t0) / 1000).toFixed(1) + " s)");
    }
    console.error("[wl-dump] actions done");
  };
  if (ACTIONS) {
    const start = setInterval(() => {
      if (announced) {
        clearInterval(start);
        runActions().catch((e) => console.error("[wl-dump] " + (e && e.stack || e)));
      }
    }, 100);
  }

  return {
    done() {
      clearInterval(timer);
      look();
      channel.close();
      console.error("[wl-dump] " + dumped + " PNGs in " + DIR + "; " + frame + " looks, " + beats + " heartbeats seen");
    },
  };
};

const text = (memory, at, size = 64) => {
  const m = new Uint8Array(memory.buffer, at, size);
  let end = 0;
  while (end < size && m[end]) {
    end++;
  }
  return new TextDecoder("utf-8", { fatal: false }).decode(m.slice(0, end));
};
