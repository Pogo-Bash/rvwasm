# Plasma shell: what the page and the server say to each other (the server's side)

The layout is the one the page proposed (kdelook's copy of this file,
`git -C ../rvwasm-wt/kdelook show kdelook:wlserver/PLASMA-PROTOCOL.md`); the
server (branch kdeshell) does all of it as written there, and the additions
marked **extra** below. Version word 3 stays; `shell_version` (1912) is 1.

## Header (1912..2047)

| offset | field | who | meaning |
| --- | --- | --- | --- |
| 1912 | shell_version | server | 1 |
| 1916, 1920, 1924 | out_strut_top, _left, _right | page | the strips kept for panels on those edges (bottom: out_panel at 1884), written with the out_* words before out_serial |
| 1928 | desktops | server | how many virtual desktops (kwinrc [Desktops] Number; 1 when none) |
| 1932 | desktop_current | server | the current one, 1-based |
| 1936 | desktop_rows | server | the pager's rows (kwinrc Rows, at most the number of desktops) |
| 1940, 1944, 1948 | desktops_lo, _hi, _len | server | UTF-8 JSON `[{ "id": "…", "name": "Desktop 1" }, …]` in order (ids are KWin's Id_N) |
| 1952 | desktops_serial | server | +1 when any of the four above changes |
| 1956 | shell_serial | server | +1 when something else here changes |
| 1960, 1964, 1968, 1972 | work_x, _y, _w, _h | server | **extra**: the usable area the server works with (the screen less the four strips) |
| 1976 | showing_desktop | server | **extra**: 1 while KWin's "show desktop" is on (the page's Peek at Desktop button can show it pressed); desktops_serial moves with it |

## Slots (232..255)

| offset | field | meaning |
| --- | --- | --- |
| 232 | desktop | 1-based; 0 on all desktops. A window's subsurfaces and popups carry their window's |
| 236 | shell_flags | 1 keep above, 2 keep below, 4 shaded, 8 skip taskbar, 16 skip switcher, 32 demands attention, 64 tiled, 128 no border (a window rule; the frame flag 32 is then off too) |
| 240 | tile | KWin's quick tile numbers: 1 left, 2 right, 4 top, 8 bottom, sums for corners, 15 maximized by a tile order; 0 none |

| 248 | layer | **extra**: a layer surface's layer (kind 6): 0 background, 1 bottom, 2 top, 3 overlay |
| 252 | role | **extra**: the window's role in Plasma's shell, as `org_kde_plasma_surface.role`: 0 normal, 1 desktop, 2 panel, 3 on-screen display, 4 notification, 5 tooltip, 6 critical notification, 7 applet popup; for a layer surface the role its namespace stands for (`desktop` 1, `dock` 2, ...) |

Windows on another desktop keep their flags; the page skips slots whose
desktop is neither 0 nor desktop_current. The server gives them no input, and
closes popups of windows that go away. Activating a window on another desktop
(the task manager, Alt-Tab) makes its desktop the current one (KWin's
default). `z` keeps KWin's layers: below < normal < above < the active
fullscreen window < popups.

## Orders

| op | id | a, b, c, d | what the server does |
| --- | --- | --- | --- |
| 11 | window | a: 1 set, 0 clear, 2 toggle | keep above (on turns keep below off) |
| 12 | window | same | keep below |
| 13 | window | same | shade (only windows with our frame; their surface gets no pointer input) |
| 14 | window | same | on all desktops (clear: on the current desktop) |
| 15 | window | a: desktop (1-based; 0 all); **extra** b: 1 = switch there too and activate it | move to that desktop |
| 16 | 0 | a: desktop (1-based) | switch to it |
| 17 | window | x, y, w, h of the window geometry | put it there at that size (a maximized window is not any more) |
| 18 | window | a: tile (0: out of the tile, the size and place from before back; 15: maximize) | quick tile within the usable area; moving the window out of its tile (order 2) also gives it its size back, at the place dragged to; a resize by hand ends the tile |
| 19 | 0 | a: count (1..20) | desktops added at the end or taken from it (windows of a desktop taken go to the one before it) |
| 20 | 0 | a: 1 on, 0 off, 2 toggle | **extra**: KWin's show desktop (Meta+D, Peek at Desktop): the current desktop's windows minimized, back when it ends; activating a window ends it |
| 21 | 0 | a: 1 left, 2 right, 3 up, 4 down, 5 next, 6 previous; b: 1 = take the active window along | **extra**: one desktop in that direction on the grid, wrapping around when kwinrc [Windows] RollOverDesktops is true (default false): for Meta+Ctrl+arrows (KWin 6.3's default), Ctrl+Alt+Left/Right (Plasma 5's), Meta+Ctrl+Shift+arrows (window along) |

KWin 6.3's default keys (src/virtualdesktops.cpp), for the page: Ctrl+F1..F4
switch to desktop 1..4 (order 16); Meta+Ctrl+Left/Right/Up/Down one desktop
(order 21).

## Notifications (**extra**; BroadcastChannel "rv-plasma")

The server owns `org.freedesktop.Notifications` on the session bus (spec 1.2,
capabilities body, body-markup, body-hyperlinks, body-images, icon-static,
actions, persistence, action-icons). Konsole (KNotification), busctl, GTK
programs send there. The page draws the popups:

    server -> page
    { type: "notify", id, appName, appIcon, summary, body, actions: [[key, label], …],
      urgency: 0 | 1 | 2, timeout: ms (-1: the page's default, 0: until dismissed),
      desktopEntry, category, resident, transient, replaces: bool, time: Date.now(),
      image: { width, height, rowstride, hasAlpha, bitsPerSample, channels, data: Uint8Array } | null,
      imagePath: "a file" | null }
    { type: "notify-close", id }            (the program closed it)

    page -> server (then ring the doorbell)
    { type: "notify-closed", id, reason }   1 expired, 2 dismissed, 3 closed
    { type: "notify-action", id, action }   an action's key; "default" for a click on the popup

`body` may have the spec's markup (b, i, u, a href, img); keep only those
tags. An icon name (appIcon) is looked up with the existing `icon` question.

## Tray icons (**extra**; same channel)

The server is `org.kde.StatusNotifierWatcher` (and the host: programs see
IsStatusNotifierHostRegistered true). The page gets

    { type: "tray", items: [{ key, id, title, status, category, iconName, iconThemePath,
                              iconPixmap: { width, height, data (ARGB32, big-endian) } | null, tooltip }] }

whenever the list or an item changes (ask `{ type: "tray-list" }` for it
again), and asks `{ type: "tray-activate", key, x, y }`,
`{ type: "tray-secondary", key, x, y }`, `{ type: "tray-scroll", key, delta,
orientation }`. Menus (dbusmenu) are not done.

## Wallpaper and the desktop containment (nothing new on the wire)

`org.kde.plasmashell` /PlasmaShell: `setWallpaper` and `wallpaper` (System
Settings' Wallpaper page), `evaluateScript` (plasma-apply-wallpaperimage and
the like: the wallpaper statements only; the script is read, never run). The
server writes `~/.config/plasma-org.kde.plasma.desktop-appletsrc` as
plasmashell does: every desktop containment of the screen gets the
`wallpaperplugin` and `[Wallpaper][PLUGIN][General]` keys (Image, FillMode,
Color as `r,g,b`, ...); the page follows that file already. At start the
server makes sure there is a desktop containment for the current activity
(kactivitymanagerdrc `[main] currentActivity`, made if missing) on screen 0,
which System Settings' Wallpaper page needs. The image's own file or package
directory may be fetched by the page wherever it is (wlserver/plasma.js lets
it), e.g. a picture in ~/Pictures.

## Window rules (nothing on the wire)

kwinrulesrc applied when a window maps (and before its first configure);
reread on `reconfigure` and on System Settings' `reloadConfig` signal. A rule
can set desktop, keep above/below, shade, skip taskbar/switcher, no border
(shell_flags 128), maximized, minimized, fullscreen, position, size.

## The splash screen (**extra**; BroadcastChannel "rv-display")

While a desktop starts the page shows Plasma's splash screen (tools/www/splash.js) instead of its plain
desktop. The server tells it its stages, by KSplash's names (plasma-workspace 6.3, ksplash/ksplashqml):

    server -> page
    { type: "splash", pid, stage }   "initial" when the server's process starts (boot.js, before it loads
                                     anything), "ksmserver" when the session's services are on the bus; and
                                     whatever a program of the session tells org.kde.KSplash /KSplash
                                     setStage(s), which the server is on the bus (ksplash.js)

The table's announcement counts as "wm". The page counts the distinct names as ksplashqml does (seven: done) and
ends the splash when its own desktop is ready (the theme built, the wallpaper decoded, the panel drawn); which
splash it draws is ksplashrc's (Engine, Theme), read with the other files.

## plasmashell as the shell (**extra**)

`desktop wayland plasma-native` starts the server with `WLSERVER_SHELL=plasma` and Plasma's own shell,
plasmashell, as its client (wlserver/layershell.js, wlserver/plasmashell.js). What changes on the wire:

- Header word 36 (`flags`, 0 before): bit 1 = plasmashell is the desktop's shell. The page then keeps no strips
  for panels of its own (out_panel and out_strut_* 0), draws no panel, and draws no wallpaper once a layer surface
  of the background layer is shown (plasmashell's desktop covers the screen). `?shell=plasma` on the page does
  the same without the server's word. The usable area (work_*) is the screen less the layer surfaces' exclusive
  zones (plasmashell's panels) and the page's strips, the larger on each edge.
- Slot kind 6: a layer surface (`zwlr_layer_surface_v1`): drawn where the slot says, as it is, never framed, not
  in the task list or the switcher, on all desktops (desktop 0). Its `layer` word says which layer; `z` already
  has it in KWin's order (desktop < below < normal < above < notification < active fullscreen < tooltip <
  critical notification < on-screen display < overlay, popups over all). A panel hidden at its screen edge
  (auto-hide) has flag 64, as a minimized window.
- Plasma's own windows (applet popups, tooltips, notifications, OSDs: xdg toplevels with a plasma role) are
  kind 1 with the `role` word, no frame (flag 32 off), skip taskbar and switcher (shell_flags 8, 16).
