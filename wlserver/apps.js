// The programs there are, for the page's launcher, and starting one.
//
// At start the server reads the .desktop files in /usr/share/applications
// (and ~/.local/share/applications) and puts what the launcher needs in the
// table, as JSON: [{ name, comment, icon, categories, file, wmclass }], in the order of their
// names. The page orders a launch by an entry's index in that list (and the
// list's serial, so that an order for an older list is not mistaken for one
// of this). The command is the entry's Exec line, split into words by the
// rules of the Desktop Entry Specification and with its field codes (%f, %u,
// ...) taken out; it is never given to a shell. It is started as
// `desktop wayland PROGRAM ARGS...`, the way a person would start it, so that
// it gets the environment and the session bus that script gives every client
// (and joins this server, which holds the lock). Its output goes to
// /root/wlserver-apps.log.
//
// Starting a program from JavaScript: hostnode's C library has hn_spawn
// (hostnode.c), which is the child between clone and exec (CLONE_VFORK: there
// is no fork on this kernel). Children are reaped every second.
(function (ws) {
  "use strict";

  const { log, sys1, sys3, sys4, SYS, table, C } = ws;
  const O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 0o100, O_APPEND = 0o2000, O_DIRECTORY = 0o200000, O_CLOEXEC = 0o2000000;
  const AT_FDCWD = -100;
  const WNOHANG = 1;
  const HN_SPAWN_DETACHED = 1;
  // (and the desktop's own, with the window server: tools/x/xdg/data/applications)
  const DIRS = ["/usr/share/applications", "/usr/local/share/applications", "/root/.local/share/applications",
                "/run/wlserver/xdg/data/applications"];
  const LOG = "/root/wlserver-apps.log";

  // The names of the files in a directory.
  const list = (dir) => {
    const fd = sys4(SYS.openat, AT_FDCWD, ws.path(dir, ws.P0), O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0) {
      return [];
    }
    const names = [];
    for (;;) {
      const n = sys3(SYS.getdents64, fd, ws.S, ws.SCRATCH);
      if (n <= 0) {
        break;
      }
      const v = ws.dv();
      for (let at = ws.S; at < ws.S + n;) {
        const reclen = v.getUint16(at + 16, true);
        const name = ws.cstring(at + 19);
        if (name !== "." && name !== "..") {
          names.push(name);
        }
        at += reclen;
      }
    }
    sys1(SYS.close, fd);
    return names;
  };

  // The [Desktop Entry] group of a .desktop file: its keys (the unlocalized ones), or null.
  const entry = (text) => {
    const keys = {};
    let inGroup = false;
    for (const raw of text.split("\n")) {
      const line = raw.trim();
      if (!line || line[0] === "#") {
        continue;
      }
      if (line[0] === "[") {
        inGroup = line === "[Desktop Entry]";
        continue;
      }
      const eq = line.indexOf("=");
      if (inGroup && eq > 0) {
        const key = line.slice(0, eq).trim();
        if (!(key in keys)) {
          keys[key] = line.slice(eq + 1).trim();
        }
      }
    }
    return Object.keys(keys).length ? keys : null;
  };

  // A string value's escapes (\s \n \t \r \\).
  const unescape = (s) => s.replace(/\\([sntr\\;])/g, (m, c) => ({ s: " ", n: "\n", t: "\t", r: "\r", "\\": "\\", ";": ";" })[c]);

  // An Exec value as words: quoted words in double quotes (with \" \` \$ \\ inside), field codes taken out.
  // null if it cannot be split.
  const words = (exec) => {
    const out = [];
    let word = null, i = 0;
    const s = unescape(exec);
    while (i < s.length) {
      const c = s[i];
      if (c === " " || c === "\t") {
        if (word !== null) {
          out.push(word);
          word = null;
        }
        i++;
      } else if (c === "\"") {
        word = word || "";
        i++;
        while (i < s.length && s[i] !== "\"") {
          if (s[i] === "\\" && i + 1 < s.length && "\"`$\\".includes(s[i + 1])) {
            i++;
          }
          word += s[i++];
        }
        if (i >= s.length) {
          return null;	// (an unterminated quote)
        }
        i++;
      } else {
        word = (word || "") + c;
        i++;
      }
    }
    if (word !== null) {
      out.push(word);
    }
    // Field codes: %% is a %, a word that is only a field code goes, others lose theirs.
    const result = [];
    for (const w of out) {
      if (/^%[fFuUdDnNickvm]$/.test(w)) {
        continue;
      }
      result.push(w.replace(/%([fFuUdDnNickvm%])/g, (m, c) => (c === "%" ? "%" : "")));
    }
    return result.length ? result : null;
  };

  let apps = [];	// [{ name, comment, icon, categories, argv }]
  let serial = 0;

  const scan = () => {
    const seen = new Set();
    const found = [];
    for (const dir of DIRS) {
      for (const file of list(dir)) {
        if (!file.endsWith(".desktop") || seen.has(file)) {
          continue;
        }
        seen.add(file);	// (the first directory's file of a name counts, as XDG_DATA_DIRS would have it)
        const text = ws.readText(dir + "/" + file);
        const e = text && entry(text);
        if (!e || (e.Type || "Application") !== "Application" || e.NoDisplay === "true" || e.Hidden === "true" || !e.Exec) {
          continue;
        }
        if (e.OnlyShowIn && !e.OnlyShowIn.split(";").includes("KDE")) {
          continue;
        }
        const argv = words(e.Exec);
        if (!argv) {
          continue;
        }
        if (e.Terminal === "true") {
          argv.unshift("konsole", "-e");	// (a program for a terminal: in one)
        }
        found.push({ name: unescape(e.Name || file.slice(0, -8)), comment: unescape(e.Comment || e.GenericName || ""),
                     icon: e.Icon || "", categories: e.Categories || "", file, wmclass: e.StartupWMClass || "", argv });
      }
    }
    found.sort((a, b) => a.name.localeCompare(b.name));
    apps = found;
    const json = JSON.stringify(apps.map((a) => ({ name: a.name, comment: a.comment, icon: a.icon, categories: a.categories,
                                                         file: a.file, wmclass: a.wmclass })));
    serial = table.apps(new TextEncoder().encode(json));
    log("apps: " + apps.length + " programs for the launcher");
  };

  // Lay out struct hn_spawn (hostnode.c) and its strings in one block; the caller frees it.
  const spawnBlock = (argv, env, fds, flags) => {
    const enc = new TextEncoder();
    const strings = [argv[0], ...argv, ...env].map((s) => enc.encode(s));
    const header = 64, pointers = 8 * (argv.length + 1 + env.length + 1), fdBytes = 4 * fds.length;
    const size = strings.reduce((n, raw) => n + raw.length + 1, 0);
    const block = ws.alloc(header + pointers + fdBytes + size + 8);
    const v = ws.dv(), m = ws.u8();
    let at = block + header + pointers + fdBytes, next = 0;
    const put = () => {
      const raw = strings[next++];
      const here = at;
      m.set(raw, here);
      m[here + raw.length] = 0;
      at += raw.length + 1;
      return here;
    };
    v.setBigUint64(block, BigInt(put()), true);		// file
    let p = block + header;
    v.setBigUint64(block + 8, BigInt(p), true);		// argv
    for (let i = 0; i < argv.length; i++, p += 8) {
      v.setBigUint64(p, BigInt(put()), true);
    }
    v.setBigUint64(p, 0n, true);
    p += 8;
    v.setBigUint64(block + 16, BigInt(p), true);		// envp
    for (let i = 0; i < env.length; i++, p += 8) {
      v.setBigUint64(p, BigInt(put()), true);
    }
    v.setBigUint64(p, 0n, true);
    p += 8;
    v.setBigUint64(block + 24, 0n, true);			// cwd: ours
    v.setBigUint64(block + 32, BigInt(p), true);		// fds
    fds.forEach((fd, i) => v.setInt32(p + 4 * i, fd, true));
    v.setInt32(block + 40, fds.length, true);
    v.setInt32(block + 44, flags, true);
    v.setInt32(block + 56, 0, true);
    return block;
  };

  // Start a program (argv, looked up in the PATH) in the session; its process number or minus an errno.
  const spawn = (argv) => {
    if (typeof C.hn_spawn !== "function") {
      return -38;	// (a hostnode without hn_spawn)
    }
    const logFd = sys4(SYS.openat, AT_FDCWD, ws.path(LOG, ws.P0), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o644);
    const block = spawnBlock(argv, ws.envp.filter((e) => !/^(HOSTNODE_HOME|WLSERVER_DEBUG|WLSERVER_STATS)=/.test(e)),
                             [-1, logFd, logFd], HN_SPAWN_DETACHED);
    const pid = Number(C.hn_spawn(BigInt(block)));
    ws.free(block);
    if (logFd >= 0) {
      sys1(SYS.close, logFd);
    }
    return pid;
  };
  ws.spawn = spawn;

  // Start a program with the environment and descriptors given (fds[i] is the child's descriptor i; -1:
  // /dev/null); its process number or minus an errno. (xwm.js starts Xwayland so.)
  ws.spawnWith = (argv, env, fds, flags = 0) => {
    if (typeof C.hn_spawn !== "function") {
      return -38;
    }
    const block = spawnBlock(argv, env, fds, flags);
    const pid = Number(C.hn_spawn(BigInt(block)));
    ws.free(block);
    return pid;
  };

  // The page's launch order: entry `index` of the list whose serial is `listSerial`.
  ws.launch = (index, listSerial, n) => {
    const app = listSerial === serial ? apps[index] : null;
    if (!app) {
      log("apps: no program " + index + " in list " + listSerial + " (the list is " + serial + ")");
      table.launched(n, 22);
      return;
    }
    const pid = spawn(["desktop", "wayland", ...app.argv]);
    log("apps: " + app.name + ": " + JSON.stringify(app.argv) + (pid > 0 ? ", process " + pid : ", error " + -pid));
    table.launched(n, pid > 0 ? 0 : -pid);
  };

  // Every second (server.js): children that have ended are let go of.
  ws.reap = () => {
    for (;;) {
      const pid = sys4(SYS.wait4, -1, ws.S, WNOHANG, 0);
      if (pid <= 0) {
        break;
      }
      if (ws.childExited) {
        ws.childExited(pid, ws.dv().getInt32(ws.S, true));	// (xwm.js: Xwayland)
      }
    }
  };

  ws.appWords = words;	// (for tests)
  try {
    scan();
  } catch (error) {
    log("apps: cannot read the programs: " + (error && error.message));
  }
})
