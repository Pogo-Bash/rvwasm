// org.kde.KSplash on the session bus, as ksplashqml has it (plasma-workspace
// 6.3: ksplash/ksplashqml/org.kde.KSplash.xml, splashapp.cpp): the object
// /KSplash with one method, setStage(s). Plasma's programs call it as they
// come up (startplasma "startPlasma", plasma-session "ready", plasmashell
// "desktop"; ksplashqml counts the distinct names it has heard, and at seven
// it is done). Here the splash is the page's (tools/www/splash.js): a stage
// a program of the session names is handed to it the same way as the
// server's own stages (boot.js, ws.splashStage), and counts there as it would
// in ksplashqml. The page ends the splash when its own desktop is ready, so a
// program that says "ready" early does not uncover a half-drawn desktop.
(function (ws) {
  "use strict";

  const { log, dbus } = ws;
  if (!dbus || !dbus.connected || !ws.splashStage) {
    return;
  }

  dbus.export("/KSplash", "org.kde.KSplash", {
    methods: {
      setStage: { in: "s", inNames: ["stage"], fn: ([stage], message) => {
        if (typeof stage !== "string" || !stage || stage.length > 64) {
          return;
        }
        log("ksplash: stage " + stage + (message && message.sender ? " from " + message.sender : ""));
        ws.splashStage(stage);
      } },
    },
  });
  dbus.own("org.kde.KSplash");
})
