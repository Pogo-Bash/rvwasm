// hostopen: hand a web address to the browser the machine runs in, so that it
// opens in the person's own browser (a tab of the host's Chrome) instead of a
// browser inside the machine. Heavy pages and video are what the machine is
// worst at: riscv64 Firefox has no JavaScript JIT and decodes video in
// software, under the emulator; the host's browser does both natively.
//
//   HOSTNODE_HOME=/run/hostopen hostnode URL      (bin/rv-host-open says it shorter)
//
// Like the window server, this is JavaScript that hostnode runs as a process
// of the machine (hostnode/hostnode.c, tools/patch-runtime.py item 11), so it
// can speak to the page: it says the address on the BroadcastChannel
// "rv-host" and waits for the page's answer. The page always asks the person
// first, in a box of its own at the top of the page ("Open example.com/... in a
// new tab?": Open / Cancel; README.md says why it has to ask).
//
// Every tab of the dev server's origin hears the channel, and each may run a
// machine of its own. So that only the page of THIS machine takes the
// address, the message carries a place in the machine's memory and a random
// mark written there: the page looks at its own machine's memory and finds
// the mark only if the message came from it. (The window server's table is
// told apart the same way: by being in the page's own machine's memory.)
//
// Exits 0 when the page has shown it to the person (or opened it), 1 when the page
// refused it (not http or https, too long, ...), 2 for a bad command line, 3
// when no page answered within three seconds (a page without hostopen.js).
(function (host) {
  "use strict";

  const memory = host.memory;
  const C = host.instance.exports;
  const K = host.syscalls;
  const SP = host.stack_pointer.value;
  const TP = C.__get_tls_base();
  const B = BigInt, N = Number;
  const sys1 = (n, a) => N(K[1](SP, TP, B(n), B(a)));
  const sys3 = (n, a, b, c) => N(K[3](SP, TP, B(n), B(a), B(b), B(c)));
  const SYS = { openat: 56, close: 57, read: 63, write: 64, fstat: 80, exit: 93, exit_group: 94, getpid: 172 };
  const sys4 = (n, a, b, c, d) => N(K[4](SP, TP, B(n), B(a), B(b), B(c), B(d)));

  const MAX_URL = 2048;
  const MARK = 32;		// bytes: "RVHO" and 28 random ones
  const ANSWER_MS = 3000;

  const u8 = () => new Uint8Array(memory.buffer);
  const dv = () => new DataView(memory.buffer);
  const encoder = new TextEncoder();
  const cstring = (address) => {
    const m = u8();
    let end = address;
    while (m[end]) {
      end++;
    }
    return new TextDecoder("utf-8").decode(m.slice(address, end));
  };
  const pointers = (address) => {
    const list = [];
    for (let at = address; ; at += 8) {
      const p = N(dv().getBigUint64(at, true));
      if (!p) {
        return list;
      }
      list.push(cstring(p));
    }
  };
  const start = N(host.arg);
  const argv = pointers(N(dv().getBigUint64(start + 8, true)));

  const scratch = N(C.malloc(B(4096)));
  const say = (fd, line) => {
    const raw = encoder.encode(line + "\n").subarray(0, 4096);
    u8().set(raw, scratch);
    sys3(SYS.write, fd, scratch, raw.length);
  };
  const exit = (code) => {
    sys1(SYS.exit_group, code);
    for (;;) {
      sys1(SYS.exit, code);
    }
  };

  // The same rules as the page's (tools/www/hostopen.js: check), so that a bad address is told here, at once.
  // The page checks again: it is the page that opens it.
  const check = (raw) => {
    if (typeof raw !== "string" || !raw.length) {
      return [null, "no address"];
    }
    if (raw.length > MAX_URL) {
      return [null, "longer than " + MAX_URL + " characters"];
    }
    if (/[\u0000- \u007f-\u009f]/.test(raw)) {
      return [null, "spaces or control characters in it"];
    }
    let url;
    try {
      url = new URL(raw);
    } catch (error) {
      return [null, "not an address"];
    }
    if (url.protocol !== "http:" && url.protocol !== "https:") {
      return [null, "only http and https addresses go to the host (not " + url.protocol + ")"];
    }
    if (url.username || url.password) {
      return [null, "a name or password in the address"];
    }
    if (!url.hostname) {
      return [null, "no host in the address"];
    }
    return [url.href, null];
  };

  // --save FILE: the file to the host, the other way from a file dropped on the screen: the page offers it as
  // a download of the browser (into the person's Downloads folder). Read here into this process's memory, and
  // the page copies it from there (the same marked-memory check as an address: only this machine's page
  // takes it). At most MAX_SAVE bytes.
  const MAX_SAVE = 512 * 1024 * 1024;
  let save = null;
  if (argv.length === 3 && argv[1] === "--save") {
    const path = argv[2];
    const raw = encoder.encode(path + "\0");
    const at = N(C.malloc(B(raw.length)));
    u8().set(raw, at);
    const fd = sys4(SYS.openat, -100, at, 0, 0);	// (AT_FDCWD, O_RDONLY)
    if (fd < 0) {
      say(2, "rv-host-save: cannot open " + path + " (errno " + -fd + ")");
      exit(1);
    }
    const st = N(C.malloc(B(256)));
    if (sys3(SYS.fstat, fd, st, 0) < 0) {
      say(2, "rv-host-save: cannot stat " + path);
      exit(1);
    }
    const mode = dv().getUint32(st + 16, true), size = N(dv().getBigInt64(st + 48, true));
    if ((mode & 0o170000) !== 0o100000) {
      say(2, "rv-host-save: not a file: " + path);
      exit(1);
    }
    if (size > MAX_SAVE) {
      say(2, "rv-host-save: " + path + " is bigger than " + (MAX_SAVE >> 20) + " MiB");
      exit(1);
    }
    const data = N(C.malloc(B(Math.max(size, 1))));
    if (!data) {
      say(2, "rv-host-save: no memory for " + size + " bytes");
      exit(1);
    }
    let got = 0;
    while (got < size) {
      const n = sys3(SYS.read, fd, data + got, Math.min(size - got, 1 << 24));
      if (n < 0) {
        say(2, "rv-host-save: cannot read " + path + " (errno " + -n + ")");
        exit(1);
      }
      if (n === 0) {
        break;
      }
      got += n;
    }
    sys1(SYS.close, fd);
    save = { name: path.split("/").pop() || "file", address: data, size: got };
  } else if (argv.length !== 2) {
    say(2, "usage: HOSTNODE_HOME=/run/hostopen hostnode URL, or hostnode --save FILE");
    exit(2);
  }
  const [url, why] = save ? [null, null] : check(argv[1]);
  if (!save && !url) {
    say(2, "rv-host-open: " + why + ": " + argv[1].slice(0, 200));
    exit(1);
  }

  // The mark, in this machine's memory.
  const mark = N(C.malloc(B(MARK)));
  const random = new Uint8Array(MARK);
  crypto.getRandomValues(random);
  random.set([0x52, 0x56, 0x48, 0x4f]);	// "RVHO"
  u8().set(random, mark);
  const id = Array.from(random.subarray(4, 12), (b) => b.toString(16).padStart(2, "0")).join("");

  // The page load this machine belongs to: rvtab=ID on the kernel's command line (tools/www/dev.html), which
  // names the channel, "rv-host-ID" (tools/www/display.js: window.rvChannel). With no id the name is plain.
  let tab = "";
  {
    const at = N(C.malloc(B(4096)));
    u8().set(encoder.encode("/proc/cmdline\0"), at);
    const fd = sys4(SYS.openat, -100, at, 0, 0);	// (AT_FDCWD, O_RDONLY)
    if (fd >= 0) {
      const n = sys3(SYS.read, fd, at, 4095);
      sys1(SYS.close, fd);
      const found = /(?:^|\s)rvtab=([0-9A-Za-z-]+)/.exec(n > 0 ? new TextDecoder().decode(u8().slice(at, at + n)) : "");
      tab = found ? found[1] : "";
    }
  }

  let channel;
  try {
    channel = new BroadcastChannel(tab ? "rv-host-" + tab : "rv-host");
  } catch (error) {
    say(2, "rv-host-open: no BroadcastChannel here (" + error.message + ")");
    exit(3);
  }
  const done = (code, line) => {
    u8().fill(0, mark, mark + MARK);	// (said once: the mark is not left to be found again)
    if (line) {
      say(code ? 2 : 1, line);
    }
    channel.close();
    exit(code);
  };
  channel.onmessage = (event) => {
    const m = event.data;
    if (m && m.type === "save-answer" && m.id === id && save) {
      if (m.status === "saved") {
        done(0, "rv-host-save: " + save.name + " (" + save.size + " bytes) is in the host's downloads");
      } else if (m.status === "asked") {
        done(0, "rv-host-save: the page asks the person to save " + save.name + " (a box at the top of the page)");
      } else {
        done(1, "rv-host-save: the page refused it: " + (m.reason || m.status));
      }
      return;
    }
    if (!m || m.type !== "open-answer" || m.id !== id) {
      return;
    }
    if (m.status === "opened") {
      done(0, "rv-host-open: opened on the host: " + url);
    } else if (m.status === "asked") {
      done(0, "rv-host-open: the page asks the person to open it (a box at the top of the page): " + url);
    } else {
      done(1, "rv-host-open: the page refused it: " + (m.reason || m.status));
    }
  };
  setTimeout(() => done(3, (save ? "rv-host-save" : "rv-host-open") + ": no page answered (is it a page with hostopen.js?)"),
             save ? ANSWER_MS + 30000 : ANSWER_MS);	// (a big file takes the page a while to copy)
  const pid = N(K[0](SP, TP, B(SYS.getpid)));
  if (save) {
    channel.postMessage({ type: "save", id, name: save.name, size: save.size, address: save.address, mark,
                          markBytes: Array.from(random), pid });
  } else {
    channel.postMessage({ type: "open", id, url, mark, markBytes: Array.from(random), pid });
  }
})
