// process, console through the guest's descriptors, synchronous fs on guest files.
const fs = require("fs");
const path = require("path");
const os = require("os");
console.log("argv", JSON.stringify(process.argv.slice(1)));
console.log("env", process.env.HN_TEST, "cwd", process.cwd(), "pid>0", process.pid > 0, "platform", process.platform, process.arch, process.version);
console.error("to stderr");
const dir = fs.mkdtempSync("/tmp/hn-");
const file = path.join(dir, "a.txt");
fs.writeFileSync(file, "héllo\n");
fs.appendFileSync(file, "world\n");
console.log("read", JSON.stringify(fs.readFileSync(file, "utf8")), fs.statSync(file).size, fs.statSync(file).isFile(), fs.existsSync(file), fs.existsSync(file + "x"));
fs.mkdirSync(path.join(dir, "d/e/f"), { recursive: true });
fs.renameSync(file, path.join(dir, "d", "b.txt"));
fs.symlinkSync("b.txt", path.join(dir, "d", "link"));
console.log("readdir", fs.readdirSync(path.join(dir, "d")).sort().join(","), fs.readlinkSync(path.join(dir, "d", "link")), fs.lstatSync(path.join(dir, "d", "link")).isSymbolicLink(), fs.realpathSync(path.join(dir, "d", "link")) === path.join(dir, "d", "b.txt"));
console.log("dirents", fs.readdirSync(path.join(dir, "d"), { withFileTypes: true }).map((d) => d.name + ":" + (d.isDirectory() ? "d" : d.isSymbolicLink() ? "l" : "f")).sort().join(","));
const fd = fs.openSync(path.join(dir, "d", "b.txt"), "r+");
const buf = Buffer.alloc(5);
fs.readSync(fd, buf, 0, 5, 7);
fs.writeSync(fd, "W", 7);
fs.closeSync(fd);
console.log("pread", buf.toString(), fs.readFileSync(path.join(dir, "d", "b.txt")).toString("latin1").length);
try { fs.readFileSync("/nonexistent/x"); } catch (e) { console.log("error", e.code, e.errno, e.syscall, e.message); }
fs.rmSync(dir, { recursive: true });
console.log("gone", !fs.existsSync(dir), os.hostname().length >= 0, os.homedir(), os.tmpdir(), os.EOL === "\n", os.cpus().length > 0, os.totalmem() > 0);
console.log(require("util").inspect({ a: [1, 2, { b: new Map([[1, 2]]) }], s: new Set([1]), d: new Date(0), r: /x/g, e: new RangeError("r").message, n: null, u: undefined, big: 10n, sym: Symbol("s"), f() {}, buf: Buffer.from("hi") }, { depth: 4, breakLength: 200 }));
process.exitCode = 7;
