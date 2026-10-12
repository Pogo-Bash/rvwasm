// timers, promises, process.nextTick, async fs, the order of things in the event loop.
const fs = require("fs");
const fsp = require("fs/promises");
const out = [];
const log = (s) => out.push(s);
setTimeout(() => log("timeout 20"), 20);
// (An immediate and a timeout of 0 set by the main program come in either order in Node.js too.)
setTimeout(() => { log("timeout 0"); setImmediate(() => log("immediate")); }, 0);
process.nextTick(() => log("tick"));
Promise.resolve().then(() => log("promise"));
queueMicrotask(() => log("microtask"));
log("sync");
let n = 0;
const iv = setInterval(() => { if (++n === 3) { clearInterval(iv); log("interval x3"); } }, 5);
const unrefd = setTimeout(() => log("never (unref)"), 100000);
unrefd.unref();
(async () => {
  await new Promise((r) => setTimeout(r, 40));
  log("await timeout 40");
  await fsp.mkdir("/tmp/hn3/a/b", { recursive: true });
  await fsp.writeFile("/tmp/hn3/a/b/f.txt", "async data");
  const text = await fsp.readFile("/tmp/hn3/a/b/f.txt", "utf8");
  const st = await fsp.stat("/tmp/hn3/a/b/f.txt");
  log("fsp " + text + " " + st.size);
  const fh = await fsp.open("/tmp/hn3/a/b/f.txt", "r");
  const { bytesRead, buffer } = await fh.read(Buffer.alloc(5), 0, 5, 6);
  await fh.close();
  log("filehandle " + bytesRead + " " + buffer.subarray(0, bytesRead).toString());
  await new Promise((resolve, reject) => fs.readFile("/tmp/hn3/a/b/f.txt", (err, data) => { if (err) reject(err); else { log("callback readFile " + data.length); resolve(); } }));
  await new Promise((resolve) => fs.readdir("/tmp/hn3/a", (err, names) => { log("callback readdir " + names); resolve(); }));
  await new Promise((resolve) => fs.stat("/tmp/nonexistent", (err) => { log("callback error " + err.code); resolve(); }));
  const rs = fs.createReadStream("/tmp/hn3/a/b/f.txt", { highWaterMark: 4 });
  let chunks = 0, total = "";
  for await (const chunk of rs) { chunks++; total += chunk; }
  log("stream " + chunks + " " + total);
  await new Promise((resolve) => { const ws = fs.createWriteStream("/tmp/hn3/w.txt"); ws.write("a"); ws.end("b", resolve); });
  log("writestream " + fs.readFileSync("/tmp/hn3/w.txt", "utf8"));
  await fsp.rm("/tmp/hn3", { recursive: true });
  const t0 = performance.now();
  await require("timers/promises").setTimeout(30);
  const dt = performance.now() - t0;
  log("timers/promises " + (dt >= 29 && dt < 200));
  // nextTick before promise jobs, also inside callbacks
  await new Promise((resolve) => setTimeout(() => {
    Promise.resolve().then(() => log("p in timer"));
    process.nextTick(() => log("tick in timer"));
    setImmediate(resolve);
  }, 1));
  console.log(out.join("\n"));
  process.on("exit", (code) => console.log("exit event", code));
  process.on("beforeExit", () => console.log("beforeExit"));
})();
