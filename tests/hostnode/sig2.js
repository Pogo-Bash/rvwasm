for (const s of ["SIGTERM", "SIGUSR1", "SIGINT", "SIGHUP"]) process.on(s, () => { console.log("got", s); if (s === "SIGHUP") process.exit(0); });
let n = 0;
setInterval(() => { if (++n == 40) { console.log("giving up"); process.exit(1); } }, 50);
console.log("ready", process.pid);
