#!/usr/bin/env node
// Measure the GPU executor prototype (tools/www/gpu-demo.html: display.js and gpu-exec.js with no machine) in
// Chrome for Testing: for each query string, load the page, let it run, take its numbers (window.demo.result())
// and a screenshot. Prints one line of JSON per run.
//
//   tools/with-browser.sh WHO node tools/gpu-demo-drive.js BASE_URL OUT_DIR 'QUERY' ['QUERY' ...]
//   e.g. ... http://localhost:8000/gpu-host build/gpu '?gpu=main&gw=1024&gh=768' '?gpu=worker&gw=1024&gh=768'
//
// Env: WARM_MS (default 3000) before the numbers are reset, RUN_MS (default 6000) measured, HEADED=1.
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

if (process.env.RV_BROWSER != "1") {
  console.error("run this under tools/with-browser.sh (one test browser at a time)");
  process.exit(2);
}
const [base, outDir, ...queries] = process.argv.slice(2);
if (!base || !outDir || !queries.length) {
  console.error("usage: gpu-demo-drive.js BASE_URL OUT_DIR QUERY...");
  process.exit(2);
}
const WARM = Number(process.env.WARM_MS || 3000), RUN = Number(process.env.RUN_MS || 6000);
const CHROME = process.env.CHROME || path.join(__dirname,
  "chrome/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  fs.mkdirSync(outDir, { recursive: true });
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "rvgpu-"));
  const chrome = spawn(CHROME, [
    ...(process.env.HEADED ? [] : ["--headless=new"]),
    "--user-data-dir=" + profile, "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check",
    "--window-size=1100,1300", "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
    "--disable-backgrounding-occluded-windows", "about:blank",
  ], { stdio: ["ignore", "ignore", "pipe"] });
  const kill = () => {
    try {
      chrome.kill();
    } catch (error) {
      // (gone already)
    }
  };
  process.on("exit", kill);
  const address = await new Promise((resolve, reject) => {
    let err = "";
    chrome.stderr.on("data", (d) => {
      err += d;
      const m = err.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) {
        resolve(m[1]);
      }
    });
    chrome.on("exit", () => reject(new Error("Chrome exited: " + err)));
  });
  const ws = new WebSocket(address);
  const waiting = new Map();
  let next = 1;
  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.method == "Runtime.exceptionThrown") {
      const d = msg.params.exceptionDetails;
      console.error("page threw: " + ((d.exception && d.exception.description) || d.text).split("\n").slice(0, 3).join(" | "));
    }
    if (msg.method == "Runtime.consoleAPICalled" && process.env.VERBOSE) {
      console.error("console: " + msg.params.args.map((a) => a.value).join(" "));
    }
    if (msg.id && waiting.has(msg.id)) {
      const { resolve, reject } = waiting.get(msg.id);
      waiting.delete(msg.id);
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
    }
  };
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = () => reject(new Error("cannot connect to Chrome"));
  });
  const send = (method, params = {}, sessionId) => {
    const id = next++;
    ws.send(JSON.stringify({ id, method, params, sessionId }));
    return new Promise((resolve, reject) => waiting.set(id, { resolve, reject }));
  };
  const { targetId } = await send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
  await send("Page.enable", {}, sessionId);
  await send("Runtime.enable", {}, sessionId);
  const evaluate = async (expression) => {
    const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }, sessionId);
    if (r.exceptionDetails) {
      throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    }
    return r.result.value;
  };
  let n = 0;
  for (const q of queries) {
    n++;
    await send("Page.navigate", { url: base + "/gpu-demo.html" + q }, sessionId);
    await sleep(WARM);
    try {
      await evaluate("window.demo.reset()");
      await sleep(RUN);
      const r = await evaluate("window.demo.result()");
      console.log(JSON.stringify(r));
      const shot = await send("Page.captureScreenshot", { format: "png" }, sessionId);
      fs.writeFileSync(path.join(outDir, "run" + n + ".png"), Buffer.from(shot.data, "base64"));
    } catch (error) {
      console.log(JSON.stringify({ url: q, error: error.message }));
    }
  }
  ws.close();
  kill();
  await sleep(500);
  try {
    fs.rmSync(profile, { recursive: true, force: true });
  } catch (error) {
    // (Chrome may still be writing its profile as it goes; the directory is in the temporary area anyway)
  }
}

main().then(() => process.exit(0), (error) => {
  console.error(error.stack || error);
  process.exit(1);
});
