const tls = require("tls");
const host = process.argv[2] || "example.com";
const t0 = Date.now();
const s = tls.connect({ host, port: 443, servername: host }, () => {
  console.log("secureConnect", s.authorized, s.getProtocol(), JSON.stringify(s.getCipher()), s.alpnProtocol, JSON.stringify(s.getPeerCertificate().subject), Date.now() - t0, "ms");
  s.write("HEAD / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
});
s.on("connect", () => console.log("tcp connected", s.remoteAddress, Date.now() - t0, "ms"));
s.setEncoding("latin1");
let got = "";
s.on("data", (d) => { got += d; });
s.on("end", () => console.log("end; first line:", got.split("\r\n")[0], Date.now() - t0, "ms"));
s.on("close", () => console.log("close"));
s.on("error", (e) => console.log("error", e.code, e.message));
setTimeout(() => { console.log("still waiting after 20 s; state", s._handle && s._handle.__state, JSON.stringify(s._handle && s._handle.__errors)); }, 20000).unref();
if (process.argv[3]) {
  let n = 0;
  const iv = setInterval(() => {
    const h = s._handle;
    console.log("tick", n, h ? [h.__state, "under", h.__under && [h.__under.__fd, h.__under.__reading, h.__under.__refed, h.__under.__active, h.__under.__closed, h.__under.bytesRead, h.__under.bytesWritten], "inner", h.__inner && [h.__inner.__fd, h.__inner.__reading, h.__inner.bytesRead, h.__inner.bytesWritten], "plain", [h.__fd, h.__reading, h.bytesRead, h.bytesWritten]].join(" ") : "no handle", JSON.stringify(h && h.__errors.slice(-200)));
    if (++n === 6) clearInterval(iv);
  }, 1000);
}
