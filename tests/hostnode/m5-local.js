// Sockets and HTTP within the machine: a server and its client in one program.
const net = require("net");
const http = require("http");
const server = net.createServer((socket) => {
  socket.on("data", (d) => socket.write(String(d).toUpperCase()));
  socket.on("end", () => socket.end(" bye"));
});
server.listen(0, "127.0.0.1", () => {
  const { port, address, family } = server.address();
  console.log("listening", address, family, port > 0);
  const client = net.connect(port, "127.0.0.1", () => {
    console.log("connected", client.remoteAddress, client.remotePort === port, client.localAddress);
    client.write("hello ");
    client.end("tcp");
  });
  let got = "";
  client.on("data", (d) => { got += d; });
  client.on("close", () => {
    console.log("tcp", JSON.stringify(got));
    server.close(httpTest);
  });
});
net.connect(1, "127.0.0.1").on("error", (e) => console.log("refused", e.code));

function httpTest() {
  const srv = http.createServer((req, res) => {
    let body = "";
    req.on("data", (d) => { body += d; });
    req.on("end", () => {
      res.writeHead(req.url === "/missing" ? 404 : 200, { "content-type": "text/plain", "x-method": req.method });
      res.write("you sent " + JSON.stringify(body) + " to " + req.url + "; ");
      res.end("agent " + req.headers["user-agent"]);
    });
  });
  srv.listen(0, "127.0.0.1", async () => {
    const port = srv.address().port;
    const get = (path, options = {}, body) => new Promise((resolve, reject) => {
      const req = http.request({ host: "127.0.0.1", port, path, ...options, headers: { "user-agent": "t" } }, (res) => {
        let text = "";
        res.setEncoding("utf8");
        res.on("data", (d) => { text += d; });
        res.on("end", () => resolve(res.statusCode + " " + res.headers["x-method"] + " " + text));
      });
      req.on("error", reject);
      req.end(body);
    });
    console.log("http", await get("/a?b=1"));
    console.log("http", await get("/post", { method: "POST" }, "a body"));
    console.log("http", await get("/missing"));
    const r = await fetch("http://127.0.0.1:" + port + "/fetch", { method: "PUT", body: "via fetch", headers: { "user-agent": "f" } });
    console.log("fetch", r.status, r.headers.get("x-method"), await r.text());
    srv.close(() => console.log("closed"));
    srv.closeAllConnections();
  });
}
