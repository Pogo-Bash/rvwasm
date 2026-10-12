// KDE's configuration files (KConfig's INI format), read and written the way
// KConfig does it, for the parts that keep KWin's and Plasma's settings
// (desktops.js: kwinrc [Desktops]; rules.js: kwinrulesrc; kwin.js: the
// wallpaper in plasma-org.kde.plasma.desktop-appletsrc).
//
// The format (KConfigIniBackend in kconfig/src/core/kconfigini.cpp): groups
// in brackets, nested groups as several bracketed names in a row
// (`[Containments][1][Wallpaper]`), `key=value` lines, `#` comments; a key
// may have a locale (`Name[de]`) or flags (`key[$e]`); values escape \ as \\,
// newlines, tabs and carriage returns as \n \t \r, a leading or trailing
// space as \s, and other control characters as \xNN. The files that count
// are /etc/xdg/NAME, then ~/.config/kdedefaults/NAME, then ~/.config/NAME,
// later ones over earlier ones; only ~/.config/NAME is written, and written
// whole to a new file that is then renamed over the old one (as KConfig's
// QSaveFile does), so that watchers (plasma.js) see one change.
//
// A parsed file: { groups: Map(group name -> Map(key -> value)) }, a group's
// name being what is between the outer brackets, `Containments][1][Wallpaper`
// for the nested one above (ws.kconfig.group(["Containments", "1", "Wallpaper"])
// makes that name).
(function (ws) {
  "use strict";

  const { sys1, sys4, sys5, SYS, path, P0, P1, log, envp } = ws;
  const O_WRONLY = 1, O_CREAT = 0o100, O_TRUNC = 0o1000, O_CLOEXEC = 0o2000000, AT_FDCWD = -100;
  const N_RENAMEAT2 = 276, N_MKDIRAT = 34;

  const home = (envp.find((e) => e.startsWith("HOME=")) || "HOME=/root").slice(5).replace(/\/+$/, "") || "/root";
  const CONFIG = home + "/.config";
  const DIRS = ["/etc/xdg", CONFIG + "/kdedefaults", CONFIG];

  const unescape = (s) => s.replace(/\\(x[0-9a-fA-F]{2}|.)/g, (m, c) => {
    if (c[0] === "x" && c.length === 3) {
      return String.fromCharCode(parseInt(c.slice(1), 16));
    }
    return { s: " ", t: "\t", n: "\n", r: "\r", "\\": "\\" }[c] !== undefined ? { s: " ", t: "\t", n: "\n", r: "\r", "\\": "\\" }[c] : "\\" + c;
  });
  const escape = (s) => {
    let out = String(s).replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/\t/g, "\\t").replace(/\r/g, "\\r")
      .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f]/g, (c) => "\\x" + c.charCodeAt(0).toString(16).padStart(2, "0"));
    if (out.startsWith(" ")) {
      out = "\\s" + out.slice(1);
    }
    if (out.endsWith(" ")) {
      out = out.slice(0, -1) + "\\s";
    }
    return out;
  };

  const parse = (text) => {
    const groups = new Map();
    let g = null;
    for (const raw of (text || "").split("\n")) {
      const line = raw.replace(/\r$/, "").trim();
      if (!line || line[0] === "#") {
        continue;
      }
      if (line[0] === "[") {
        const end = line.lastIndexOf("]");
        const name = end > 0 ? line.slice(1, end) : line.slice(1);
        if (name === "$i") {
          continue;	// (the whole file immutable: not followed here)
        }
        if (!groups.has(name)) {
          groups.set(name, new Map());
        }
        g = groups.get(name);
        continue;
      }
      const eq = line.indexOf("=");
      if (eq <= 0) {
        continue;
      }
      if (!g) {
        if (!groups.has("<default>")) {
          groups.set("<default>", new Map());
        }
        g = groups.get("<default>");
      }
      g.set(line.slice(0, eq).trim(), unescape(line.slice(eq + 1).replace(/^[ \t]+/, "")));
    }
    return { groups };
  };

  const serialize = (cfg) => {
    const out = [];
    const def = cfg.groups.get("<default>");
    if (def && def.size) {
      for (const [k, v] of def) {
        out.push(k + "=" + escape(v));
      }
      out.push("");
    }
    for (const [name, keys] of cfg.groups) {
      if (name === "<default>" || !keys.size) {
        continue;
      }
      out.push("[" + name + "]");
      for (const [k, v] of keys) {
        out.push(k + "=" + escape(v));
      }
      out.push("");
    }
    return out.join("\n");
  };

  const group = (parts) => (Array.isArray(parts) ? parts.join("][") : parts);

  const kconfig = {
    home, CONFIG, DIRS, parse, serialize, group, escape, unescape,
    // One file as parsed (its text from `dir`, ~/.config by default); an empty one if it is not there.
    readFile(name, dir = CONFIG) {
      return parse(ws.readText(dir + "/" + name) || "");
    },
    // The file as KDE's programs see it: the system's, the look-and-feel defaults and the user's together.
    read(name) {
      const merged = { groups: new Map() };
      for (const dir of DIRS) {
        const cfg = parse(ws.readText(dir + "/" + name) || "");
        for (const [g, keys] of cfg.groups) {
          if (!merged.groups.has(g)) {
            merged.groups.set(g, new Map());
          }
          const m = merged.groups.get(g);
          for (const [k, v] of keys) {
            m.set(k.replace(/\[\$[a-z]+\]$/, ""), v);
          }
        }
      }
      return merged;
    },
    get(cfg, g, key, fallback = null) {
      const keys = cfg.groups.get(group(g));
      return keys && keys.has(key) ? keys.get(key) : fallback;
    },
    bool(cfg, g, key, fallback) {
      const v = kconfig.get(cfg, g, key, null);
      return v === null ? fallback : /^(true|1|on|yes)$/i.test(v.trim());
    },
    int(cfg, g, key, fallback) {
      const v = kconfig.get(cfg, g, key, null);
      const n = v === null ? NaN : parseInt(v, 10);
      return isNaN(n) ? fallback : n;
    },
    set(cfg, g, key, value) {
      const name = group(g);
      if (!cfg.groups.has(name)) {
        cfg.groups.set(name, new Map());
      }
      cfg.groups.get(name).set(key, String(value));
    },
    remove(cfg, g, key) {
      const keys = cfg.groups.get(group(g));
      if (keys) {
        keys.delete(key);
      }
    },
    // The names of the groups under a group (`Containments` -> ["1", "2", ...]).
    children(cfg, g) {
      const prefix = group(g) + "][";
      const names = new Set();
      for (const name of cfg.groups.keys()) {
        if (name.startsWith(prefix)) {
          names.add(name.slice(prefix.length).split("][")[0]);
        }
      }
      return [...names];
    },
    // Write ~/.config/NAME whole: a new file renamed over the old one. True if it worked.
    write(name, cfg) {
      sys4(N_MKDIRAT, AT_FDCWD, path(CONFIG, P0), 0o700, 0);
      const file = CONFIG + "/" + name;
      const tmp = file + ".wlserver-new";
      const fd = sys4(SYS.openat, AT_FDCWD, path(tmp, P0), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o600);
      if (fd < 0) {
        log("kconfig: cannot write " + tmp + " (error " + -fd + ")");
        return false;
      }
      const r = ws.writeAll(fd, serialize(cfg));
      sys1(SYS.close, fd);
      if (r < 0) {
        log("kconfig: writing " + tmp + ": error " + -r);
        return false;
      }
      path(tmp, P0);
      path(file, P1);
      const m = sys5(N_RENAMEAT2, AT_FDCWD, P0, AT_FDCWD, P1, 0);
      if (m < 0) {
        log("kconfig: renaming " + tmp + " over " + file + ": error " + -m);
        return false;
      }
      return true;
    },
  };
  ws.kconfig = kconfig;
})
