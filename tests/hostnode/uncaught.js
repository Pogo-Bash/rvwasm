process.on("uncaughtException", (e) => { console.log("caught", e.message); });
setTimeout(() => { throw new Error("later"); }, 1);
setTimeout(() => { console.log("still running"); process.exit(3); }, 20);
