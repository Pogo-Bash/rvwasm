// node-machine --hook for the Widgets test: tools/wl-dump.js (the window server's table, PNGs) and dl-hook.js (the module
// cache's statistics) together.
const wlDump = require("../../tools/wl-dump.js");
const dlHook = require("./dl-hook.js");
module.exports = (os, info) => {
  const a = wlDump(os, info);
  const b = dlHook(os, info);
  return {
    done: async () => {
      if (a && a.done) {
        await a.done();
      }
      return b.done();
    },
  };
};
