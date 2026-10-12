// The keyboard's layout and its modifiers.
//
// The page sends physical keys (evdev codes, from KeyboardEvent.code), so a
// layout is nothing but the XKB keymap the clients are given: they turn keys
// into characters with it themselves (libxkbcommon, and its compose tables
// for dead keys). The keymaps are files in keymaps/ (tools/wl-keymaps.sh
// makes them with xkbcli from the Debian root's xkb-data; keymaps/index.txt
// lists them), and the page says which one it wants (table.js: layout).
// When that changes, every keyboard is sent the new keymap.
//
// What a server must do itself is keep the modifiers' state, because it
// sends them (wl_keyboard.modifiers: depressed, latched, locked, group) and
// the clients take them as they are. Which modifier a key sets depends on
// the keymap: in `us` the right Alt key is Alt (Mod1), in `de` it is AltGr
// (ISO_Level3_Shift, Mod5). So this reads, from the keymap's text, the
// keycodes, the first symbol of every key and the modifier map, and works
// out each key's action the way xkb-data's compat rules do for the keys
// that have one: Shift, Control, Alt, Meta, Super, Hyper set the real
// modifiers of their key's modifier_map; ISO_Level3_Shift and
// ISO_Level5_Shift set LevelThree and LevelFive, whatever real modifier the
// keys of that name are mapped to (Mod5 and Mod3 in the keymaps here), and
// their _Latch and _Lock forms latch or lock it; Caps_Lock locks Lock,
// Shift_Lock Shift, Num_Lock NumLock (Mod2). Group switching (one layout per
// keymap here) and the less common actions are not done.
(function (ws) {
  "use strict";

  const { log, sys1, sys2, sys3, sys4, SYS } = ws;
  const O_RDWR = 2, O_CREAT = 0o100, O_EXCL = 0o200, O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const SHIFT = 1, LOCK = 2, CONTROL = 4, MOD1 = 8, MOD2 = 16, MOD3 = 32, MOD4 = 64, MOD5 = 128;
  const REAL = { Shift: SHIFT, Lock: LOCK, Control: CONTROL, Mod1: MOD1, Mod2: MOD2, Mod3: MOD3, Mod4: MOD4, Mod5: MOD5 };
  const SET = 1, LATCH = 2, LOCKS = 3;

  // The symbols that make a key a modifier key: what it does, and where its modifiers come from ("map": its
  // own modifier_map; a virtual modifier's name: whatever the keys bearing that name's symbols are mapped to).
  const ACTIONS = {
    Shift_L: [SET, "map", SHIFT], Shift_R: [SET, "map", SHIFT],
    Control_L: [SET, "map", CONTROL], Control_R: [SET, "map", CONTROL],
    Alt_L: [SET, "map", MOD1], Alt_R: [SET, "map", MOD1], Meta_L: [SET, "map", MOD1], Meta_R: [SET, "map", MOD1],
    Super_L: [SET, "map", MOD4], Super_R: [SET, "map", MOD4], Hyper_L: [SET, "map", MOD4], Hyper_R: [SET, "map", MOD4],
    ISO_Level3_Shift: [SET, "LevelThree", MOD5], ISO_Level3_Latch: [LATCH, "LevelThree", MOD5],
    ISO_Level3_Lock: [LOCKS, "LevelThree", MOD5],
    ISO_Level5_Shift: [SET, "LevelFive", MOD3], ISO_Level5_Latch: [LATCH, "LevelFive", MOD3],
    ISO_Level5_Lock: [LOCKS, "LevelFive", MOD3],
    Caps_Lock: [LOCKS, "real", LOCK], Shift_Lock: [LOCKS, "real", SHIFT], Num_Lock: [LOCKS, "NumLock", MOD2],
  };
  const VIRTUAL = { LevelThree: ["ISO_Level3_Shift", "ISO_Level3_Latch", "ISO_Level3_Lock"],
                    LevelFive: ["ISO_Level5_Shift", "ISO_Level5_Latch", "ISO_Level5_Lock"], NumLock: ["Num_Lock"] };

  // A keymap's text, read: { actions: Map(evdev code -> [kind, mods]) }.
  const parse = (text) => {
    const codes = new Map();	// key name -> evdev code
    const keycodes = /xkb_keycodes[^{]*\{([\s\S]*?)\n\};/.exec(text);
    if (keycodes) {
      for (const m of keycodes[1].matchAll(/^\s*<([^>]+)>\s*=\s*(\d+)\s*;/gm)) {
        codes.set(m[1], Number(m[2]) - 8);
      }
      for (const m of keycodes[1].matchAll(/^\s*alias\s+<([^>]+)>\s*=\s*<([^>]+)>\s*;/gm)) {
        if (codes.has(m[2])) {
          codes.set(m[1], codes.get(m[2]));
        }
      }
    }
    const symbols = /xkb_symbols[^{]*\{([\s\S]*?)\n\};/.exec(text);
    const first = new Map();	// key name -> its first symbol (group 1, level 1)
    const all = new Map();	// key name -> all its symbols in group 1
    const modmap = new Map();	// key name -> real modifiers
    if (symbols) {
      for (const m of symbols[1].matchAll(/^\s*key\s+<([^>]+)>\s*\{([\s\S]*?)\};/gm)) {
        const body = m[2];
        const group = /symbols\[Group1\]\s*=\s*\[([^\]]*)\]/.exec(body) || /\[([^\]]*)\]/.exec(body);
        if (group) {
          const syms = group[1].split(",").map((x) => x.trim()).filter(Boolean);
          first.set(m[1], syms[0]);
          all.set(m[1], syms);
        }
      }
      const bySym = new Map();
      for (const [name, syms] of all) {
        for (const sym of syms) {
          if (!bySym.has(sym)) {
            bySym.set(sym, []);
          }
          bySym.get(sym).push(name);
        }
      }
      for (const m of symbols[1].matchAll(/^\s*modifier_map\s+(\w+)\s*\{([^}]*)\}/gm)) {
        const mod = REAL[m[1]] || 0;
        for (let item of m[2].split(",").map((x) => x.trim()).filter(Boolean)) {
          const names = item.startsWith("<") ? [item.slice(1, -1)] : bySym.get(item) || [];
          for (const name of names) {
            modmap.set(name, (modmap.get(name) || 0) | mod);
          }
        }
      }
    }
    // The virtual modifiers, as real ones: the modifier_map of the keys whose first symbol is theirs.
    const virtual = {};
    for (const [vmod, syms] of Object.entries(VIRTUAL)) {
      let mods = 0;
      for (const [name, sym] of first) {
        if (syms.includes(sym)) {
          mods |= modmap.get(name) || 0;
        }
      }
      virtual[vmod] = mods;
    }
    const actions = new Map();
    for (const [name, sym] of first) {
      const a = ACTIONS[sym];
      const code = codes.get(name);
      if (!a || code === undefined) {
        continue;
      }
      let mods;
      if (a[1] === "map") {
        mods = modmap.get(name) || a[2];
      } else if (a[1] === "real") {
        mods = a[2];
      } else {
        mods = virtual[a[1]] || a[2];
      }
      actions.set(code, [a[0], mods]);
    }
    return { actions, virtual };
  };

  // ---- the layouts there are

  const index = ws.readText(ws.home + "/keymaps/index.txt") || "";
  const layouts = index.split("\n").filter(Boolean).map((line) => {
    const [name, description] = line.split("\t");
    return { name, description: description || name };
  });
  ws.layouts = layouts;

  const runtimeDir = (ws.envp.find((e) => e.startsWith("XDG_RUNTIME_DIR=")) || "XDG_RUNTIME_DIR=/tmp").slice(16);
  let files = 0;

  // A keymap as the file the clients are sent (unlinked: only its descriptor is passed around), or null.
  const load = (name) => {
    if (!/^[a-z0-9_-]{1,31}$/.test(name) || !layouts.some((l) => l.name === name)) {
      return null;
    }
    const text = ws.readFile(ws.home + "/keymaps/" + name + ".xkb");
    if (!text) {
      log("keyboard: cannot read keymaps/" + name + ".xkb");
      return null;
    }
    const data = new Uint8Array(text.length + 1);	// (with the NUL libxkbcommon wants at the end)
    data.set(text);
    const file = runtimeDir + "/.wlserver-keymap-" + sys1(SYS.getpid, 0) + "-" + ++files;
    const fd = sys4(SYS.openat, AT_FDCWD, ws.path(file, ws.P0), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0o600);
    if (fd < 0) {
      log("keyboard: cannot make the keymap's file " + file + " (error " + -fd + ")");
      return null;
    }
    // (Grown from size 0 by ftruncate first: on this kernel that gives the file contiguous pages, which a
    // client mapping it shared needs.)
    sys2(SYS.ftruncate, fd, data.length);
    ws.writeAll(fd, data);
    sys3(SYS.unlinkat, AT_FDCWD, ws.path(file, ws.P0), 0);
    const parsed = parse(new TextDecoder().decode(text));
    return { name, fd, size: data.length, actions: parsed.actions, virtual: parsed.virtual };
  };

  // ---- the state

  const kbd = {
    keymap: null,
    depressed: 0, latched: 0, locked: 0, group: 0,
    down: new Map(),		// modifier keys held: code -> [kind, mods]
    latchPending: 0,		// a latch key held, not yet released

    // Make `name` the layout. Returns false if there is no such layout (the old one stays).
    use(name) {
      const keymap = load(name);
      if (!keymap) {
        return false;
      }
      if (kbd.keymap) {
        sys1(SYS.close, kbd.keymap.fd);	// (clients that were sent it have their own descriptors)
      }
      kbd.keymap = keymap;
      // The modifiers start again (keys held now are taken as they come up): only the locks stay.
      kbd.down.clear();
      kbd.depressed = 0;
      kbd.latched = 0;
      log("keyboard: layout " + name + " (AltGr is " + modName(keymap.virtual.LevelThree) + ", NumLock "
          + modName(keymap.virtual.NumLock) + ")");
      return true;
    },

    // A key went down or up: true if the modifiers changed.
    key(code, pressed) {
      const before = [kbd.depressed, kbd.latched, kbd.locked].join();
      const action = kbd.keymap && kbd.keymap.actions.get(code);
      if (action) {
        const [kind, mods] = action;
        if (pressed) {
          kbd.down.set(code, action);
          if (kind === LOCKS) {
            kbd.locked ^= mods;
          } else if (kind === LATCH) {
            kbd.latchPending |= mods;
          }
        } else {
          kbd.down.delete(code);
          if (kind === LATCH && (kbd.latchPending & mods)) {
            kbd.latched |= mods;	// (let go with nothing pressed meanwhile: it holds for the next key)
            kbd.latchPending &= ~mods;
          }
        }
        let d = 0;
        for (const [k, m] of kbd.down.values()) {
          if (k === SET || k === LATCH) {
            d |= m;
          }
        }
        kbd.depressed = d;
      } else if (!pressed && kbd.latched) {
        kbd.latched = 0;	// (a latch is used by the next key that is not a modifier)
      } else if (pressed) {
        kbd.latchPending = 0;	// (a key pressed while a latch key is held: it was held, not latched)
      }
      return [kbd.depressed, kbd.latched, kbd.locked].join() !== before;
    },

    // All keys are up (the page lost the keyboard): nothing held any more.
    reset() {
      kbd.down.clear();
      kbd.depressed = 0;
      kbd.latched = 0;
      kbd.latchPending = 0;
    },
  };

  const modName = (mods) => Object.keys(REAL).filter((n) => mods & REAL[n]).join("+") || "nothing";

  kbd.parse = parse;	// (for tests)
  ws.kbd = kbd;
  if (!layouts.length) {
    log("keyboard: no keymaps/index.txt: keyboards get no keymap");
  } else if (!kbd.use("us")) {
    kbd.use(layouts[0].name);
  }
})
