// child_process, signals, standard input.
const cp = require("child_process");
const r = cp.spawnSync("/bin/echo", ["hello", "sync"]);
console.log("spawnSync", r.status, r.signal, JSON.stringify(r.stdout.toString()), r.stderr.length, r.pid > 0);
console.log("execSync", JSON.stringify(cp.execSync("echo one; echo two 1>&2; (exit 3) || true", { stdio: ["ignore", "pipe", "ignore"] , shell: "/bin/sh"}).toString()) );
try { cp.execSync("exit 5", { shell: "/bin/sh" }); } catch (e) { console.log("execSync status", e.status); }
console.log("input", JSON.stringify(cp.spawnSync("/bin/cat", [], { input: "fed to cat" }).stdout.toString()));
console.log("enoent", cp.spawnSync("/bin/does-not-exist").error.code);
console.log("env+cwd", cp.spawnSync("/bin/sh", ["-c", "echo $HNV; pwd"], { env: { HNV: "v", PATH: "/bin" }, cwd: "/bin" }).stdout.toString().trim().split("\n").join(","));
console.log("timeout", cp.spawnSync("/bin/sleep", ["5"], { timeout: 100 }).error.code);
const child = cp.spawn("/bin/cat");
let got = "";
child.stdout.on("data", (d) => { got += d; });
child.on("exit", (code, signal) => console.log("spawn exit", code, signal));
child.on("close", (code) => {
  console.log("spawn close", code, JSON.stringify(got));
  const sleeper = cp.spawn("/bin/sleep", ["30"], { stdio: "inherit" });
  sleeper.on("exit", (code, signal) => {
    console.log("killed", code, signal);
    cp.exec("echo from exec; echo err 1>&2; exit 2", { shell: "/bin/sh" }, (err, stdout, stderr) => {
      console.log("exec", err && err.code, JSON.stringify(stdout), JSON.stringify(stderr));
      cp.spawn("/bin/nonexistent").on("error", (e) => {
        console.log("spawn error", e.code);
        process.on("SIGUSR1", () => {
          console.log("got SIGUSR1");
          process.on("SIGTERM", () => { console.log("got SIGTERM from child"); process.exit(0); });
          cp.spawn("/bin/sh", ["-c", "kill -TERM " + process.pid], { stdio: "inherit" });
        });
        process.kill(process.pid, "SIGUSR1");
      });
    });
  });
  setTimeout(() => sleeper.kill("SIGTERM"), 30);
});
child.stdin.write("through ");
child.stdin.end("a pipe");
