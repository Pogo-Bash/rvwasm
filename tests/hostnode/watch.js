// fs.watch (inotify) and fs.watchFile (a timer): a file changed, a directory with a file added and removed.
const fs = require("fs");
const path = require("path");
const dir = fs.mkdtempSync("/tmp/watch-");
const file = path.join(dir, "a.txt");
fs.writeFileSync(file, "one");
const seen = [];
const w1 = fs.watch(file, (type, name) => seen.push("file " + type + " " + name));
const w2 = fs.watch(dir, (type, name) => seen.push("dir " + type + " " + name));
const w3 = fs.watch(dir, { encoding: "buffer" }, (type, name) => seen.push("buf " + type + " " + Buffer.isBuffer(name)));
let polled = [];
fs.watchFile(file, { interval: 50 }, (now, before) => polled.push(before.size + "->" + now.size));
setTimeout(() => {
  fs.appendFileSync(file, " two");
  setTimeout(() => {
    fs.writeFileSync(path.join(dir, "b.txt"), "x");
    setTimeout(() => {
      fs.unlinkSync(path.join(dir, "b.txt"));
      setTimeout(() => {
        w1.close();
        w2.close();
        w3.close();
        fs.unwatchFile(file);
        const uniq = [...new Set(seen)];
        console.log(uniq.filter((s) => s.startsWith("file")).join(", "));
        console.log(uniq.filter((s) => s.startsWith("dir")).join(", "));
        console.log(uniq.filter((s) => s.startsWith("buf")).join(", "));
        console.log("polled", polled.join(" "));
        fs.rmSync(dir, { recursive: true });
      }, 200);
    }, 100);
  }, 100);
}, 100);
