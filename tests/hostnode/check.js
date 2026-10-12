// Run the hostnode tests on the machine, one line per check:
//   hostnode /usr/lib/hostnode/tests/check.js [NAME...]     (in Debian: /run/hostnode/...)
// Each check runs a program with this same hostnode and compares what it printed and how it ended with
// what Node.js prints and how it ends (the texts here are from Node 20 on Linux).
const cp = require("child_process");
const path = require("path");
const fs = require("fs");
const here = __dirname;
const node = process.execPath;
const only = process.argv.slice(2);
let failed = 0;

const check = (name, args, expected, options = {}) => {
  if (only.length && !only.includes(name)) {
    return;
  }
  const started = Date.now();
  const r = cp.spawnSync(node, args, { encoding: "utf8", env: { ...process.env, ...options.env }, input: options.input,
                                       cwd: options.cwd, timeout: options.timeout || 60000 });
  let out = (r.stdout || "") + (options.stderr ? r.stderr || "" : "");
  if (options.filter) {
    out = options.filter(out);
  }
  const status = r.status === null ? String(r.signal || (r.error && r.error.code)) : r.status;
  const wantStatus = options.status || 0;
  const good = typeof expected === "function" ? expected(out) : out === expected;
  if (good && status === wantStatus) {
    console.log("ok " + name + " (" + (Date.now() - started) + " ms)");
  } else {
    failed++;
    console.log("FAIL " + name + ": status " + status + " (want " + wantStatus + ")");
    if (!good) {
      console.log("  printed:  " + JSON.stringify(out));
      if (typeof expected === "string") {
        console.log("  expected: " + JSON.stringify(expected));
      }
    }
    if (r.stderr && !options.stderr) {
      console.log("  stderr:   " + JSON.stringify(r.stderr.slice(0, 2000)));
    }
  }
};

fs.mkdirSync("/tmp", { recursive: true });

// M1
check("eval", ["-e", "console.log(1 + 1)"], "2\n");
check("print", ["-p", "6 * 7"], "42\n");
check("exit-status", ["-e", "process.exit(3)"], "", { status: 3 });
check("version", ["--version"], "v20.19.2\n");

// M2
check("process-and-fs", [path.join(here, "m2.js"), "one", "two three"],
  'argv ["' + path.join(here, "m2.js") + '","one","two three"]\n' +
  "env yes cwd /tmp pid>0 true platform linux riscv64 v20.19.2\n" +
  'read "héllo\\nworld\\n" 13 true true false\n' +
  "readdir b.txt,e,link b.txt true true\n" +
  "dirents b.txt:f,e:d,link:l\n" +
  "pread world 13\n" +
  "error ENOENT -2 open ENOENT: no such file or directory, open '/nonexistent/x'\n" +
  "gone true true HOME /tmp true true true\n" +
  "{\n  a: [ 1, 2, { b: Map(1) { 1 => 2 } } ],\n  s: Set(1) { 1 },\n  d: 1970-01-01T00:00:00.000Z,\n  r: /x/g,\n" +
  "  e: 'r',\n  n: null,\n  u: undefined,\n  big: 10n,\n  sym: Symbol(s),\n  f: [Function: f],\n  buf: <Buffer 68 69>\n}\n",
  { status: 7, env: { HN_TEST: "yes", HOME: "HOME" }, cwd: "/tmp" });
check("stderr", ["-e", "console.error('to stderr'); console.log('to stdout')"], "to stdout\nto stderr\n", { stderr: true });
check("uncaught", ["-e", "throw new RangeError('out')"],
  (out) => /RangeError: out\n {4}at /.test(out) && out.endsWith("\n\nNode.js v20.19.2\n"), { stderr: true, status: 1 });
check("module-not-found", ["/tmp/no-such-file.js"], (out) => out.includes("Cannot find module '/tmp/no-such-file.js'"),
  { stderr: true, status: 1 });
check("stdin", ["-e", "let s = ''; process.stdin.on('data', (d) => { s += d; }).on('end', () => console.log(s.toUpperCase()))"],
  "FED IN\n", { input: "fed in" });

// M3
check("event-loop", [path.join(here, "m3.js")],
  "sync\ntick\npromise\nmicrotask\ntimeout 0\nimmediate\ntimeout 20\ninterval x3\nawait timeout 40\n" +
  "fsp async data 10\nfilehandle 4 data\ncallback readFile 10\ncallback readdir b\ncallback error ENOENT\n" +
  "stream 3 async data\nwritestream ab\ntimers/promises true\ntick in timer\np in timer\nbeforeExit\nexit event 0\n");
check("unhandled-rejection", [path.join(here, "rejection.js")],
  (out) => out.startsWith("Error: boom\n    at main (") || out.includes("\nError: boom\n    at main ("), { stderr: true, status: 1 });
check("uncaught-handler", [path.join(here, "uncaught.js")], "caught later\nstill running\n", { status: 3 });
check("exit-code-and-event", ["-e", "setTimeout(() => {}, 30); process.on('exit', (c) => console.log('bye', c)); process.exitCode = 4"],
  "bye 4\n", { status: 4 });
check("benchmark-runs", [path.join(here, "jsbench.js")], (out) => /^JSBENCH loop \d+ ms, objects \d+ ms, strings \d+ ms, floats \d+ ms, loop \d+ ms\n$/.test(out));

// M4
check("child-process", [path.join(here, "m4.js")],
  'spawnSync 0 null "hello sync\\n" 0 true\nexecSync "one\\n"\nexecSync status 5\ninput "fed to cat"\nenoent ENOENT\n' +
  "env+cwd v,/bin\ntimeout ETIMEDOUT\nspawn exit 0 null\nspawn close 0 \"through a pipe\"\nkilled null SIGTERM\n" +
  'exec 2 "from exec\\n" "err\\n"\nspawn error ENOENT\ngot SIGUSR1\ngot SIGTERM from child\n');
check("signals-from-child", [path.join(here, "sig3.js"), "idle"],
  (out) => /^idle child exit 0 u1 [1-9]\d* u2 [1-9]\d* term [1-9]\d*\n/.test(out) || /later {2}u1 [1-9]\d* u2 [1-9]\d* term [1-9]\d*\n$/.test(out));
check("killed-by-signal", ["-e", "process.kill(process.pid, 'SIGTERM'); setTimeout(() => {}, 1000)"], "", { status: "SIGTERM" });

// M6
check("watchers", [path.join(here, "watch.js")],
  "file change a.txt\ndir change a.txt, dir rename b.txt, dir change b.txt\nbuf change true, buf rename true\npolled 3->7\n");

check("random-uuid", ["-p", "const c = require('crypto'); [c.randomUUID(), crypto.randomUUID(), c.randomUUID({ disableEntropyCache: true })].every((u) => /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u))"], "true\n");

console.log(failed ? failed + " FAILED" : "all ok");
process.exit(failed ? 1 : 0);
