const cp = require("child_process");
let u1 = 0, u2 = 0, term = 0;
process.on("SIGUSR1", () => u1++);
process.on("SIGUSR2", () => u2++);
process.on("SIGTERM", () => term++);
const mode = process.argv[2];
const c = cp.spawn("/bin/sh", ["-c", "i=0; while [ $i -lt 20 ]; do kill -USR1 $P; kill -USR2 $P; kill -TERM $P; i=$((i+1)); done"], { stdio: "inherit", env: { P: String(process.pid) } });
c.on("exit", (code) => { console.log(mode, "child exit", code, "u1", u1, "u2", u2, "term", term); setTimeout(() => { console.log(mode, "later  u1", u1, "u2", u2, "term", term); process.exit(0); }, 100); });
if (mode === "busy") setInterval(() => { require("fs").statSync("/"); }, 1);
