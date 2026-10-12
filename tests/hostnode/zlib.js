// Compression: against the machine's own gzip, and round trips.
const zlib = require("zlib");
const cp = require("child_process");
const fs = require("fs");
const big = fs.readFileSync(process.argv[2] || __filename);
const text = Buffer.concat([big, big, Buffer.from("x".repeat(100000)), big]);
const sum = (b) => { let a = 1, c = 0; for (const x of b) { a = (a + x) % 65521; c = (c + a) % 65521; } return (c << 16 | a) >>> 0; };
// theirs -> ours
const theirs = cp.spawnSync("/bin/gzip", ["-c"], { input: text, maxBuffer: 1 << 28 }).stdout;
console.log("gunzip of gzip's", sum(zlib.gunzipSync(theirs)) === sum(text), theirs.length < text.length / 2);
// ours -> theirs
const ours = zlib.gzipSync(text);
const back = cp.spawnSync("/bin/gzip", ["-dc"], { input: ours, maxBuffer: 1 << 28 }).stdout;
console.log("gzip's gunzip of ours", sum(back) === sum(text), ours.length < text.length);
// round trips of every kind
for (const [name, pack, unpack] of [["deflate", zlib.deflateSync, zlib.inflateSync], ["raw", zlib.deflateRawSync, zlib.inflateRawSync], ["gzip", zlib.gzipSync, zlib.unzipSync], ["level 0", (b) => zlib.deflateSync(b, { level: 0 }), zlib.inflateSync]]) {
  console.log(name, sum(unpack(pack(text))) === sum(text), unpack(pack(Buffer.alloc(0))).length);
}
try { zlib.gunzipSync(theirs.subarray(0, theirs.length - 9)); } catch (e) { console.log("truncated", e.code); }
try { zlib.inflateSync(Buffer.from("not zlib at all")); } catch (e) { console.log("garbage", e.code); }
// streams, in small pieces
const gunzip = zlib.createGunzip();
let n = 0, a = 1, c = 0;
gunzip.on("data", (d) => { n += d.length; for (const x of d) { a = (a + x) % 65521; c = (c + a) % 65521; } });
gunzip.on("end", () => {
  console.log("stream", n === text.length, ((c << 16 | a) >>> 0) === sum(text));
  zlib.gzip("async", (err, packed) => zlib.gunzip(packed, (err2, out) => console.log("callbacks", String(out))));
});
for (let i = 0; i < theirs.length; i += 1000) gunzip.write(theirs.subarray(i, i + 1000));
gunzip.end();
