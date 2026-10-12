// The terminal: raw mode, keys as they are typed, the size of the window.
const out = process.stdout;
console.log("isTTY", process.stdin.isTTY, out.isTTY, "size", out.columns + "x" + out.rows, out.getWindowSize().join("x"));
process.stdin.setRawMode(true);
process.stdin.on("data", (d) => {
  const bytes = [...d].join(",");
  out.write("key " + bytes + "\r\n");
  if (d[0] === 113) {
    process.stdin.setRawMode(false);
    console.log("cooked again");
    process.exit(0);
  }
});
console.log("READY");
