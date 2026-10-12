// An https GET from a Node program: https.get and fetch.
const https = require("https");
const url = process.argv[2] || "https://example.com/";
const t0 = Date.now();
https.get(url, (res) => {
  let n = 0, head = "";
  const protocol = res.socket.getProtocol(), authorized = res.socket.authorized;
  res.on("data", (d) => { n += d.length; if (head.length < 200) head += d; });
  res.on("end", async () => {
    console.log("https.get", res.statusCode, protocol, authorized, n > 100, /<title>|<html|<!doctype/i.test(head), Date.now() - t0, "ms");
    const t1 = Date.now();
    const r = await fetch(url);
    const text = await r.text();
    console.log("fetch", r.status, r.headers.get("content-type"), text.length > 100, Date.now() - t1, "ms");
    const t2 = Date.now();
    const r2 = await fetch(url);
    console.log("fetch again", r2.status, (await r2.arrayBuffer()).byteLength > 100, Date.now() - t2, "ms");
    try { await fetch("https://wrong.host.badssl.com/"); console.log("bad certificate accepted!"); } catch (e) { console.log("wrong host:", e.cause ? e.cause.code || e.cause.message : e.message); }
    process.exit(0);
  });
}).on("error", (e) => { console.log("error", e.code, e.message); process.exit(1); });
