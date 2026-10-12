// The surface table: what the page draws, as a struct in the machine's
// memory (README.md has the layout, field by field; keep the two together).
//
// The page cannot be sent a message for every frame (the main thread takes
// only the runtime's own messages from a Worker), and the window server
// cannot be woken by one without a Worker that waits for messages; so the
// two share a table instead. The server writes it; the page reads it on
// every frame it draws and writes back what it has uploaded and shown; the
// server reads that on every turn. Where the table is, the server says once
// on a BroadcastChannel ("rv-display") and again every second, for a page
// that was not listening yet.
//
// A slot is written under a sequence word: odd while the server is in the
// middle of it, even otherwise, and the page reads a slot again when the
// word changed under it or was odd.
(function (ws) {
  "use strict";

  const { alloc, dv, u8, sys0, SYS, log } = ws;

  const MAGIC = 0x4c445652;	// "RVDL"
  const VERSION = 3;
  const SLOTS = 64;
  const SLOT_SIZE = 384;
  const HEADER = 2048;
  const MAX_DAMAGE = 8;
  const TEXT = 64;		// bytes for a title, and for an app id

  // Header
  const H_MAGIC = 0, H_VERSION = 4, H_PID = 8, H_HEARTBEAT = 12, H_SLOTS = 16, H_SLOT_SIZE = 20,
        H_SCREEN_W = 24, H_SCREEN_H = 28, H_GENERATION = 32, H_FLAGS = 36, H_GUEST_SEL_SERIAL = 40,
        H_IME_STATE = 80, H_IME_X = 84, H_IME_Y = 88, H_IME_H = 92,
        H_TEXT_SERIAL = 1980, H_TEXT_ACK = 1984, H_TEXT_LEN = 1988, H_TEXT = 1992, TEXT_CAP = 56,
        H_PAGE_FRAME = 64, H_PAGE_TIME = 68, H_POINTER_X = 72, H_POINTER_Y = 76,
        // the page while it is not shown: the frames a second the server is to stand in for it with (0: it is shown)
        H_PAGE_HIDDEN = 44,
        // the pointer's picture: what the page shows over the screen
        H_CURSOR_SERIAL = 96, H_CURSOR_KIND = 100, H_CURSOR_SHAPE = 104, H_CURSOR_LO = 108, H_CURSOR_HI = 112,
        H_CURSOR_W = 116, H_CURSOR_H = 120, H_CURSOR_STRIDE = 124, H_CURSOR_HOT_X = 128, H_CURSOR_HOT_Y = 132,
        // the frames the page draws around decorated windows
        H_TITLE_HEIGHT = 136, H_BORDER = 140,
        // the clipboard, both ways
        H_GUEST_CLIP_SERIAL = 144, H_GUEST_CLIP_LO = 148, H_GUEST_CLIP_HI = 152, H_GUEST_CLIP_LEN = 156,
        H_HOST_CLIP_LO = 160, H_HOST_CLIP_HI = 164, H_HOST_CLIP_CAP = 168, H_HOST_CLIP_SERIAL = 172,
        H_HOST_CLIP_LEN = 176, H_HOST_CLIP_ACK = 180,
        H_FOCUS = 184,
        // the screen: what the page wants (its size in CSS pixels, its scale), and what the server has made of it
        H_OUT_SERIAL = 188, H_OUT_W = 192, H_OUT_H = 196, H_OUT_SCALE = 200, H_OUT_ACK = 204, H_SCREEN_SCALE = 208,
        // the keyboard's layout: the page names one, the server says which it has
        H_KBD_SERIAL = 212, H_KBD_ACK = 216, H_KBD_STATUS = 220, H_KBD_NAME = 224,
        // the orders ring: the page's requests (activate, move, resize, close, ...)
        H_ORDER_HEAD = 256, H_ORDER_TAIL = 260, H_ORDERS = 264;
  const ORDERS = 32, ORDER_SIZE = 32;
  // After the orders ring (264 + 32 * 32 = 1288): the layout the keyboard has, and files from the host.
  const H_KBD_CURRENT = 1288,
        H_XFER_LO = 1320, H_XFER_HI = 1324, H_XFER_CAP = 1328, H_XFER_SERIAL = 1332, H_XFER_LEN = 1336,
        H_XFER_FLAGS = 1340, H_XFER_TOTAL = 1344, H_XFER_X = 1348, H_XFER_Y = 1352, H_XFER_ACK = 1356,
        H_XFER_STATUS = 1360, H_XFER_NAME = 1368, H_XFER_PATH = 1624, H_DRAG = 1880,
        // the page's panel (the strip at the bottom it keeps for itself), and the programs it can start
        H_OUT_PANEL = 1884, H_APPS_LO = 1888, H_APPS_HI = 1892, H_APPS_LEN = 1896, H_APPS_SERIAL = 1900,
        H_LAUNCH_ACK = 1904, H_LAUNCH_STATUS = 1908,
        // Plasma's shell (PLASMA-PROTOCOL.md): the version of that, the panels' strips on the other edges (the
        // page's), virtual desktops, a serial for the rest; the usable area and "showing the desktop"
        H_SHELL_VERSION = 1912, H_OUT_STRUT_TOP = 1916, H_OUT_STRUT_LEFT = 1920, H_OUT_STRUT_RIGHT = 1924,
        H_DESKTOPS = 1928, H_DESKTOP_CURRENT = 1932, H_DESKTOP_ROWS = 1936, H_DESKTOPS_LO = 1940, H_DESKTOPS_HI = 1944,
        H_DESKTOPS_LEN = 1948, H_DESKTOPS_SERIAL = 1952, H_SHELL_SERIAL = 1956, H_WORK = 1960, H_SHOWING_DESKTOP = 1976;
  const SHELL_VERSION = 1;
  const NAME = 32, LONG_NAME = 256;
  const XFER_CAP = 4 << 20;
  const HOST_CLIP_CAP = 1 << 20;
  // Slot
  const S_SEQ = 0, S_ID = 4, S_PARENT = 8, S_KIND = 12, S_X = 16, S_Y = 20, S_W = 24, S_H = 28, S_Z = 32,
        S_FLAGS = 36, S_GEOM = 40, S_PRESENT = 56, S_BUFFER_LO = 60, S_BUFFER_HI = 64, S_STRIDE = 68,
        S_FORMAT = 72, S_BUF_W = 76, S_BUF_H = 80, S_SCALE = 84, S_DAMAGE_COUNT = 88, S_DAMAGE = 92,
        S_UPLOADED = 224, S_SHOWN = 228, S_DESKTOP = 232, S_SHELL_FLAGS = 236, S_TILE = 240, S_GPU_SERIAL = 244,
        S_LAYER = 248, S_ROLE = 252, S_TITLE = 256, S_APP_ID = 320;

  const encoder = new TextEncoder();

  const address = alloc(HEADER + SLOTS * SLOT_SIZE);
  u8().fill(0, address, address + HEADER + SLOTS * SLOT_SIZE);
  const pid = sys0(SYS.getpid);
  {
    const v = dv();
    v.setUint32(address + H_VERSION, VERSION, true);
    v.setUint32(address + H_PID, pid, true);
    v.setUint32(address + H_SLOTS, SLOTS, true);
    v.setUint32(address + H_SLOT_SIZE, SLOT_SIZE, true);
    v.setUint32(address + H_SCREEN_W, ws.screen.width, true);
    v.setUint32(address + H_SCREEN_H, ws.screen.height, true);
    v.setUint32(address + H_TITLE_HEIGHT, ws.chrome.title, true);
    v.setUint32(address + H_BORDER, ws.chrome.border, true);
    v.setUint32(address + H_MAGIC, MAGIC, true);
  }
  // Where the page puts text from the host's clipboard for the guest.
  const hostClip = alloc(HOST_CLIP_CAP);
  {
    const v = dv();
    v.setUint32(address + H_HOST_CLIP_LO, hostClip % 0x100000000, true);
    v.setUint32(address + H_HOST_CLIP_HI, Math.floor(hostClip / 0x100000000), true);
    v.setUint32(address + H_HOST_CLIP_CAP, HOST_CLIP_CAP, true);
  }
  // Where the page puts the pieces of files dropped on it from the host (read by data.js: files).
  const xfer = alloc(XFER_CAP);
  {
    const v = dv();
    v.setUint32(address + H_XFER_LO, xfer % 0x100000000, true);
    v.setUint32(address + H_XFER_HI, Math.floor(xfer / 0x100000000), true);
    v.setUint32(address + H_XFER_CAP, XFER_CAP, true);
    v.setUint32(address + H_SCREEN_SCALE, ws.screen.scale, true);
    v.setUint32(address + H_SHELL_VERSION, SHELL_VERSION, true);
  }
  let cursorSerial = 0, guestClipSerial = 0, guestSelSerial = 0, guestClip = 0, guestClipCap = 0, lastHostSerial = 0;
  let lastOutSerial = 0, lastKbdSerial = 0, lastXferSerial = 0, lastTextSerial = 0;

  const free = [];
  for (let i = SLOTS - 1; i >= 0; i--) {
    free.push(i);
  }
  let generation = 0;

  // Atomics over the shared memory, for the sequence words (and the page's, which it writes with them).
  const words = () => new Int32Array(ws.memory.buffer);

  const slotAt = (slot) => address + HEADER + slot * SLOT_SIZE;

  const table = {
    address, pid, SLOTS, SLOT_SIZE, HEADER, MAX_DAMAGE, ORDERS, ORDER_SIZE,

    // A free slot's number, or -1.
    take() {
      if (!free.length) {
        return -1;
      }
      generation++;
      return free.pop();
    },

    // The whole slot, under its sequence word. s: id, parent, kind, x, y, w, h, z, flags, geometry [x, y, w, h],
    // present, buffer (address), stride, format, bufferWidth, bufferHeight, scale, damage [[x, y, w, h], ...] or
    // null for everything, title, appId.
    write(slot, s) {
      const at = slotAt(slot);
      const w = words();
      const v = dv();
      const seq = Atomics.add(w, at / 4, 1) + 1;	// odd: being written
      v.setUint32(at + S_ID, s.id, true);
      v.setUint32(at + S_PARENT, s.parent, true);
      v.setUint32(at + S_KIND, s.kind, true);
      v.setInt32(at + S_X, s.x, true);
      v.setInt32(at + S_Y, s.y, true);
      v.setUint32(at + S_W, s.w, true);
      v.setUint32(at + S_H, s.h, true);
      v.setUint32(at + S_Z, s.z, true);
      v.setUint32(at + S_FLAGS, s.flags, true);
      for (let i = 0; i < 4; i++) {
        v.setInt32(at + S_GEOM + 4 * i, s.geometry[i], true);
      }
      v.setUint32(at + S_PRESENT, s.present, true);
      v.setUint32(at + S_BUFFER_LO, s.buffer % 0x100000000, true);
      v.setUint32(at + S_BUFFER_HI, Math.floor(s.buffer / 0x100000000), true);
      v.setUint32(at + S_STRIDE, s.stride, true);
      v.setUint32(at + S_FORMAT, s.format, true);
      v.setUint32(at + S_BUF_W, s.bufferWidth, true);
      v.setUint32(at + S_BUF_H, s.bufferHeight, true);
      v.setUint32(at + S_SCALE, s.scale, true);
      v.setUint32(at + S_GPU_SERIAL, s.gpuSerial || 0, true);
      if (s.damage === null) {
        v.setUint32(at + S_DAMAGE_COUNT, 0xffffffff, true);
      } else {
        let rects = s.damage;
        if (rects.length > MAX_DAMAGE) {
          // Beyond the room there is: the box around them all.
          let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
          for (const r of rects) {
            x0 = Math.min(x0, r[0]);
            y0 = Math.min(y0, r[1]);
            x1 = Math.max(x1, r[0] + r[2]);
            y1 = Math.max(y1, r[1] + r[3]);
          }
          rects = [[x0, y0, x1 - x0, y1 - y0]];
        }
        v.setUint32(at + S_DAMAGE_COUNT, rects.length, true);
        rects.forEach((r, i) => {
          for (let k = 0; k < 4; k++) {
            v.setInt32(at + S_DAMAGE + 16 * i + 4 * k, r[k], true);
          }
        });
      }
      v.setUint32(at + S_DESKTOP, (s.desktop || 0) >>> 0, true);
      v.setUint32(at + S_SHELL_FLAGS, (s.shellFlags || 0) >>> 0, true);
      v.setUint32(at + S_TILE, (s.tile || 0) >>> 0, true);
      v.setUint32(at + S_LAYER, (s.layer || 0) >>> 0, true);
      v.setUint32(at + S_ROLE, (s.role || 0) >>> 0, true);
      text(at + S_TITLE, s.title);
      text(at + S_APP_ID, s.appId);
      Atomics.store(w, at / 4, seq + 1);	// even: whole
    },

    // Only the page's part: what it has uploaded and shown of this slot.
    read(slot) {
      const at = slotAt(slot);
      const w = words();
      return { uploaded: Atomics.load(w, (at + S_UPLOADED) / 4) >>> 0, shown: Atomics.load(w, (at + S_SHOWN) / 4) >>> 0 };
    },

    // The header's flags word (README.md): 1 the desktop's shell is a program of the guest's (plasmashell): the
    // page draws no panel and no wallpaper of its own.
    flags(value) {
      dv().setUint32(address + H_FLAGS, value >>> 0, true);
    },

    // The slot is nobody's again.
    release(slot) {
      const at = slotAt(slot);
      const w = words();
      const seq = Atomics.add(w, at / 4, 1) + 1;
      u8().fill(0, at + 4, at + SLOT_SIZE);
      Atomics.store(w, at / 4, seq + 1);
      free.push(slot);
      generation++;
    },

    // Once per turn: the server is alive; and the layout counter, when windows came or went.
    heartbeat() {
      const w = words();
      Atomics.add(w, (address + H_HEARTBEAT) / 4, 1);
      Atomics.store(w, (address + H_GENERATION) / 4, generation);
    },

    // The page's frame counter, as it last wrote it.
    pageFrame() {
      return Atomics.load(words(), (address + H_PAGE_FRAME) / 4) >>> 0;
    },

    // While the browser does not show the page (another tab or app in front, the window minimized, ...) it draws
    // no frames and so acknowledges no presents: the rate, in frames a second, at which the server is to count
    // them as shown in its place (0: the page is shown, and says itself).
    pageHidden() {
      const rate = Atomics.load(words(), (address + H_PAGE_HIDDEN) / 4);
      return rate > 0 && rate <= 1000 ? rate : 0;
    },

    // The pointer's picture: kind 0 none, 1 a shape (wp_cursor_shape_device_v1's numbers), 2 an image of
    // w x h pixels (ARGB8888, premultiplied) at `image`, with its hot spot.
    cursor(kind, shape, image, w, h, stride, hotX, hotY) {
      const v = dv();
      v.setUint32(address + H_CURSOR_KIND, kind, true);
      v.setUint32(address + H_CURSOR_SHAPE, shape || 0, true);
      v.setUint32(address + H_CURSOR_LO, (image || 0) % 0x100000000, true);
      v.setUint32(address + H_CURSOR_HI, Math.floor((image || 0) / 0x100000000), true);
      v.setUint32(address + H_CURSOR_W, w || 0, true);
      v.setUint32(address + H_CURSOR_H, h || 0, true);
      v.setUint32(address + H_CURSOR_STRIDE, stride || 0, true);
      v.setInt32(address + H_CURSOR_HOT_X, hotX || 0, true);
      v.setInt32(address + H_CURSOR_HOT_Y, hotY || 0, true);
      Atomics.store(words(), (address + H_CURSOR_SERIAL) / 4, ++cursorSerial);
    },

    // A guest program took the clipboard (any types, text or not): the page's paste then leaves it be unless
    // the host's clipboard may have changed since (README.md: guest_sel_serial).
    guestSelection() {
      Atomics.store(words(), (address + H_GUEST_SEL_SERIAL) / 4, ++guestSelSerial);
    },

    // Text a guest put on its clipboard, for the page to give to the host's (navigator.clipboard.writeText).
    guestClipboard(bytes) {
      if (bytes.length + 1 > guestClipCap) {
        if (guestClip) {
          ws.free(guestClip);
        }
        guestClipCap = Math.max(bytes.length + 1, 4096);
        guestClip = alloc(guestClipCap);
      }
      u8().set(bytes, guestClip);
      const v = dv();
      v.setUint32(address + H_GUEST_CLIP_LO, guestClip % 0x100000000, true);
      v.setUint32(address + H_GUEST_CLIP_HI, Math.floor(guestClip / 0x100000000), true);
      v.setUint32(address + H_GUEST_CLIP_LEN, bytes.length, true);
      Atomics.store(words(), (address + H_GUEST_CLIP_SERIAL) / 4, ++guestClipSerial);
    },

    // Text the page put there from the host's clipboard since we last looked, as bytes; or null.
    hostClipboard() {
      const w = words();
      const serial = Atomics.load(w, (address + H_HOST_CLIP_SERIAL) / 4) >>> 0;
      if (serial === lastHostSerial) {
        return null;
      }
      lastHostSerial = serial;
      const len = Math.min(Atomics.load(w, (address + H_HOST_CLIP_LEN) / 4) >>> 0, HOST_CLIP_CAP);
      return { serial, bytes: u8().slice(hostClip, hostClip + len) };
    },
    // ... and now the guest has it: the page may go on (it holds back the key that pastes until then).
    hostClipboardTaken(serial) {
      Atomics.store(words(), (address + H_HOST_CLIP_ACK) / 4, serial);
    },

    // The window that has the keyboard (its surface id), for the page to show as such.
    focus(id) {
      Atomics.store(words(), (address + H_FOCUS) / 4, id);
    },

    // Whether the window with the keyboard takes text from an input method now (textinput.js), and where its
    // text cursor is on the screen (for the page's composing box): state 1 and the cursor, or 0.
    ime(state, x = 0, y = 0, h = 0) {
      const v = dv();
      v.setInt32(address + H_IME_X, x, true);
      v.setInt32(address + H_IME_Y, y, true);
      v.setInt32(address + H_IME_H, h, true);
      Atomics.store(words(), (address + H_IME_STATE) / 4, state);
    },

    // Text the page's input method committed since we last looked (a piece of at most TEXT_CAP bytes of
    // UTF-8, acknowledged here so that the page sends the next), or null.
    imeText() {
      const w = words();
      const serial = Atomics.load(w, (address + H_TEXT_SERIAL) / 4) >>> 0;
      if (serial === lastTextSerial) {
        return null;
      }
      lastTextSerial = serial;
      const len = Math.min(TEXT_CAP, dv().getUint32(address + H_TEXT_LEN, true));
      const bytes = u8().slice(address + H_TEXT, address + H_TEXT + len);
      Atomics.store(w, (address + H_TEXT_ACK) / 4, serial);
      return bytes;
    },

    // The screen the page wants, if it asked for another since we last looked: { serial, width, height, scale }.
    output() {
      const w = words();
      const serial = Atomics.load(w, (address + H_OUT_SERIAL) / 4) >>> 0;
      if (serial === lastOutSerial) {
        return null;
      }
      lastOutSerial = serial;
      const v = dv();
      return { serial, width: v.getUint32(address + H_OUT_W, true), height: v.getUint32(address + H_OUT_H, true),
               scale: v.getUint32(address + H_OUT_SCALE, true), panel: v.getUint32(address + H_OUT_PANEL, true),
               struts: { top: v.getUint32(address + H_OUT_STRUT_TOP, true), left: v.getUint32(address + H_OUT_STRUT_LEFT, true),
                         right: v.getUint32(address + H_OUT_STRUT_RIGHT, true) } };
    },
    // ... and the screen it is now (whether or not that is what was asked for).
    outputDone(serial) {
      const v = dv();
      v.setUint32(address + H_SCREEN_W, ws.screen.width, true);
      v.setUint32(address + H_SCREEN_H, ws.screen.height, true);
      v.setUint32(address + H_SCREEN_SCALE, ws.screen.scale, true);
      Atomics.store(words(), (address + H_OUT_ACK) / 4, serial);
    },

    // The keyboard layout the page asked for, if it asked since we last looked: { serial, name }.
    layout() {
      const w = words();
      const serial = Atomics.load(w, (address + H_KBD_SERIAL) / 4) >>> 0;
      if (serial === lastKbdSerial) {
        return null;
      }
      lastKbdSerial = serial;
      return { serial, name: readText(address + H_KBD_NAME, NAME) };
    },
    // ... done (status 0) or not (1: no such layout here); `current` is the layout the keyboard has now.
    layoutDone(serial, status, current) {
      text(address + H_KBD_CURRENT, current, NAME);
      dv().setUint32(address + H_KBD_STATUS, status, true);
      Atomics.store(words(), (address + H_KBD_ACK) / 4, serial);
    },

    // A piece of a file from the host, if the page put one there since we last looked:
    // { serial, bytes (a view, valid until the next turn), flags, total, x, y, name }.
    transfer() {
      const w = words();
      const serial = Atomics.load(w, (address + H_XFER_SERIAL) / 4) >>> 0;
      if (serial === lastXferSerial) {
        return null;
      }
      lastXferSerial = serial;
      const v = dv();
      const len = Math.min(v.getUint32(address + H_XFER_LEN, true), XFER_CAP);
      return { serial, address: xfer, length: len, bytes: u8().subarray(xfer, xfer + len), flags: v.getUint32(address + H_XFER_FLAGS, true),
               total: v.getUint32(address + H_XFER_TOTAL, true), x: v.getInt32(address + H_XFER_X, true),
               y: v.getInt32(address + H_XFER_Y, true), name: readText(address + H_XFER_NAME, LONG_NAME) };
    },
    // ... taken: status 0, or an errno; `path` where the file went.
    transferDone(serial, status, path) {
      text(address + H_XFER_PATH, path || "", LONG_NAME);
      dv().setUint32(address + H_XFER_STATUS, status, true);
      Atomics.store(words(), (address + H_XFER_ACK) / 4, serial);
    },

    // The programs there are, as UTF-8 JSON (apps.js), for the page's launcher: a new list, a new serial.
    apps(bytes) {
      const at = alloc(bytes.length + 1);
      u8().set(bytes, at);
      const v = dv();
      const old = v.getUint32(address + H_APPS_LO, true) + v.getUint32(address + H_APPS_HI, true) * 0x100000000;
      v.setUint32(address + H_APPS_LO, at % 0x100000000, true);
      v.setUint32(address + H_APPS_HI, Math.floor(at / 0x100000000), true);
      v.setUint32(address + H_APPS_LEN, bytes.length, true);
      const serial = (v.getUint32(address + H_APPS_SERIAL, true) + 1) >>> 0;
      Atomics.store(words(), (address + H_APPS_SERIAL) / 4, serial);
      if (old) {
        ws.free(old);	// (a page that reads it now may read garbage once: it parses it again on the next serial)
      }
      return serial;
    },
    appsSerial() {
      return dv().getUint32(address + H_APPS_SERIAL, true);
    },
    // A launch order carried out: the order's number (the ring's count) and 0, or an errno.
    launched(n, status) {
      dv().setUint32(address + H_LAUNCH_STATUS, status, true);
      Atomics.store(words(), (address + H_LAUNCH_ACK) / 4, n);
    },

    // The virtual desktops (desktops.js): the words (current is 0-based here, 1-based in the table), and the list
    // as UTF-8 JSON, under a new serial.
    desktops(current, count, rows, bytes, showing) {
      const v = dv();
      const at = alloc(bytes.length + 1);
      u8().set(bytes, at);
      const old = v.getUint32(address + H_DESKTOPS_LO, true) + v.getUint32(address + H_DESKTOPS_HI, true) * 0x100000000;
      v.setUint32(address + H_DESKTOP_CURRENT, current + 1, true);
      v.setUint32(address + H_DESKTOPS, count, true);
      v.setUint32(address + H_SHOWING_DESKTOP, showing ? 1 : 0, true);
      v.setUint32(address + H_DESKTOP_ROWS, rows, true);
      v.setUint32(address + H_DESKTOPS_LO, at % 0x100000000, true);
      v.setUint32(address + H_DESKTOPS_HI, Math.floor(at / 0x100000000), true);
      v.setUint32(address + H_DESKTOPS_LEN, bytes.length, true);
      const serial = (v.getUint32(address + H_DESKTOPS_SERIAL, true) + 1) >>> 0;
      Atomics.store(words(), (address + H_DESKTOPS_SERIAL) / 4, serial);
      if (old) {
        ws.free(old);	// (as with the apps list: a page reading it now reads it again on the next serial)
      }
    },
    // The usable area (the screen less the panels' strips), for the page.
    workArea(a) {
      const v = dv();
      for (let i = 0; i < 4; i++) {
        v.setInt32(address + H_WORK + 4 * i, a[i] | 0, true);
      }
    },
    // Something the page has no other way to see changed (PLASMA-PROTOCOL.md: shell_serial).
    shellChanged() {
      const v = dv();
      Atomics.store(words(), (address + H_SHELL_SERIAL) / 4, (v.getUint32(address + H_SHELL_SERIAL, true) + 1) >>> 0);
    },

    // A drag is going on (1: a drag between windows), for the page to show.
    drag(state) {
      Atomics.store(words(), (address + H_DRAG) / 4, state);
    },

    // The page's orders since we last looked: [{ op, id, a, b, c, d }, ...].
    orders() {
      const w = words();
      const head = Atomics.load(w, (address + H_ORDER_HEAD) / 4) >>> 0;
      let tail = Atomics.load(w, (address + H_ORDER_TAIL) / 4) >>> 0;
      if (head === tail) {
        return null;
      }
      const v = dv();
      const list = [];
      if (((head - tail) >>> 0) > ORDERS) {
        tail = (head - ORDERS) >>> 0;	// (the page wrote over what we had not read: the newest are kept)
      }
      while (tail !== head) {
        const at = address + H_ORDERS + (tail % ORDERS) * ORDER_SIZE;
        list.push({ op: v.getUint32(at, true), id: v.getUint32(at + 4, true), a: v.getInt32(at + 8, true),
                    b: v.getInt32(at + 12, true), c: v.getInt32(at + 16, true), d: v.getInt32(at + 20, true),
                    n: (tail + 1) >>> 0 });
        tail = (tail + 1) >>> 0;
      }
      Atomics.store(w, (address + H_ORDER_TAIL) / 4, tail);
      return list;
    },

    // Tell the page (or whoever listens) where the table is.
    announce() {
      if (channel) {
        channel.postMessage({ type: "table", address, pid, slots: SLOTS, slotSize: SLOT_SIZE, header: HEADER,
                              width: ws.screen.width, height: ws.screen.height, scale: ws.screen.scale,
                              version: VERSION, layouts: ws.layouts || [] });
      }
    },
  };

  const text = (at, s, size = TEXT) => {
    const m = u8();
    m.fill(0, at, at + size);
    if (s) {
      let raw = encoder.encode(s);
      if (raw.length > size - 1) {
        raw = raw.subarray(0, size - 1);	// (may cut a character: the page's decoder is told not to mind)
      }
      m.set(raw, at);
    }
  };
  // What the page wrote as text: UTF-8, NUL-padded, in `size` bytes.
  const readText = (at, size) => {
    const m = u8();
    let end = at;
    while (end < at + size && m[end]) {
      end++;
    }
    return new TextDecoder("utf-8").decode(m.slice(at, end));
  };

  let channel = null;
  try {
    channel = ws.channel("rv-display");
  } catch (error) {
    log("table: no BroadcastChannel here (" + error.message + "): the page will not find the table");
  }

  log("table: at " + address + " (" + SLOTS + " slots of " + SLOT_SIZE + " bytes), pid " + pid);
  ws.table = table;
})
