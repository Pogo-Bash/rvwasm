// node-machine --hook: after the command, print what the page's module cache did (os.dl, patch-runtime.py item 26):
//   DLSTATS {"<key>": {"size": N, "compile": N, "shared": N, "preload": N, ...}, ...}
module.exports = (os) => ({
  done() {
    process.stdout.write("DLSTATS " + JSON.stringify(os.dl.stats()) + "\n");
  },
});
