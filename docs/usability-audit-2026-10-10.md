# Daily-driver usability audit, 2026-10-10

What a new user hits on the Plasma desktop (`dev.html?disk=daily`, `debian desktop wayland konsole`, then the
launcher), tried as a person would, in the test browser (Chrome for Testing, headless, a 1440x900 window at
devicePixelRatio 2, as on a Retina Mac). Base: main 144aa1e. Three browser sessions of about 30 minutes each,
driven step by step (`tools/desktop-drive.js repl:DIR`, added for this), plus node-machine and native rv runs
for what did not need the browser. Screenshots in `usability-audit-2026-10-10/`.

Ranked by severity times how often a person meets it (5 = every session / blocks the task). "Fixed" items
are on branch `audit`, one commit each, verified as written in each entry.

## Ranked list

| # | what | severity x frequency | state |
| --- | --- | --- | --- |
| 1 | Opening any file does nothing (Dolphin double-click or Enter, Kate's Open With, `kioclient exec`, `xdg-open` of a file) | 5 x 5 | fixed |
| 2 | Copying, pasting or dragging files in Dolphin fails (paste gets the Mac's old text; KIO's copy fails with "Errno: 38") | 5 x 4 | fixed (copy/paste); drag between windows open |
| 3 | Every new window opens at a third of the screen on a Retina Mac (Dolphin 474x284, Kate 552x287, Settings 641x383 on 1424x800) | 3 x 5 | fixed |
| 4 | No way to get a file out of the machine onto the Mac | 4 x 3 | fixed |
| 5 | No way to type Japanese, Chinese or Korean (no input method anywhere) | 5 x 2 (CJK users: every keystroke) | fixed |
| 6 | Trackpad scrolling throws the view: every wheel event was a whole notch, 30-60 per swipe | 3 x 4 | fixed |
| 7 | Drag and drop between windows (Dolphin to Dolphin) does nothing: the target never accepts | 3 x 3 | open |
| 8 | KWin/Plasma keys missing: Meta+arrows tiling, Meta+PgUp/PgDn, Ctrl+Alt+T, Meta+E, Alt+F2 | 2 x 4 | fixed |
| 9 | No image viewer that works: images open in Firefox (30 s cold); gpicview segfaults under rv | 3 x 3 | open |
| 10 | No PDF viewer installed (no Okular); Firefox has pdf.js | 2 x 3 | open (disk image) |
| 11 | Keys typed right after a shortcut are lost or land in the wrong place (Dolphin Ctrl+L then a path gave /root/root/audit; F2 then a name: rename did not happen) | 3 x 2 | open (program latency) |
| 12 | No log out / shut down / restart anywhere (launcher has no Leave buttons); no lock screen | 2 x 3 | open |
| 13 | htop (and ps-style tools) show VIRT/RES 16.0E and MEM% 100 for every rv process | 2 x 2 | open (kernel) |
| 14 | Everything runs as root: Dolphin's red "Running Dolphin as root can be dangerous" bar every time | 1 x 5 | open (disk image) |
| 15 | No audio path at all (no PipeWire/PulseAudio, ALSA has no card) | 3 x 2 | open |
| 16 | Firefox: the first-run Welcome page and Privacy Notice tab take over the first address typed (every fresh profile) | 1 x 2 | open (image: policies.json) |
| 17 | System Settings' window is a second icon next to its pinned launcher in the task manager (app id kdesystemsettings vs systemsettings.desktop) | 1 x 3 | open |
| 18 | Wallpaper page: one wallpaper, its thumbnail not drawn after 20 s | 1 x 1 | open |
| 19 | notify-send missing (libnotify-bin); a gpicview-only image association | 1 x 1 | open (image) |
| 20 | Firefox's window is 1332x818 at y=7 on an 800-high usable area: it reaches under the panel | 1 x 2 | open |

What worked as a person expects (no finding): Konsole tabs (Ctrl+Shift+T), copy to the Mac (Ctrl+Shift+C,
host clipboard got "copyline-xyz"), paste from the Mac (Ctrl+Shift+V), vim, htop, scrollback; Kate open dialog,
edit, save, find; window move by the title bar, resize by the corner, snap to half by dragging to the edge,
maximize by double-click, minimize/restore from the task manager, Alt+Tab, Alt+F4; the launcher's favourites,
categories and search; virtual-desktop keys (one desktop configured, so Ctrl+F2 does nothing, as in Plasma);
the clock's calendar; the notification history; Breeze Dark from System Settings (the page's frames and panel
follow in 0.7 s); the bar's Fullscreen and a resized browser window (windows refitted); CJK text rendering in
Kate, Konsole and Firefox; `apt-get install sl` (18.6 s).

## Timings (seconds, this test browser, the Mac busy with other agents)

| what | time |
| --- | --- |
| Page load to the first Konsole window (boot, Debian, window server, Konsole) | 14.4 |
| Dolphin, cold (panel or launcher search) | 5.1-10.3 |
| Kate, cold, from the launcher | 5.7-6.3 |
| Kate opening a file double-clicked in Dolphin (after fix 1) | 17 |
| Second Konsole (Ctrl+Alt+T) | 4 |
| Dolphin by Meta+E | 6.7 |
| System Settings, cold / second time | 27 / 10.7 |
| Firefox, cold, to a window | 29.9-31.4 |
| Firefox, a local file:// page | 11.5-16.7 |
| Kate's Open dialog | 1.2 |
| apt-get install sl (network, dpkg, man-db trigger) | 18.6 |
| Theme change to the page's frames and panel | 0.7 |

These are for the speed work (chromeperf and the JIT agents); nothing here was chased.

## The findings in detail

### 1. Opening a file did nothing (fixed: 7a5bd2e)

Double-clicking `notes.txt` in Dolphin selected it; nothing opened in 90 s. `kioclient exec` printed
`Error: "The name org.freedesktop.impl.portal.desktop.kde was not provided by any .service files"` and exited 1
(`a13-kioclient.jpg`). With `QT_LOGGING_RULES=kf.service.services=true`: `query for mimeType "text/plain" returning
0 offers` for every type. KService's cache (ksycoca) is built from the XDG menu, and Plasma's is
`plasma-applications.menu`; startplasma sets `XDG_MENU_PREFIX=plasma-`, `desktop wayland` did not, so the cache
had no programs at all (kbuildsycoca6 natively: "Menu applications.menu not found", 225 KB; with the prefix 356
KB). The desktop script now sets the prefix and rebuilds the cache once. After: Kate opens the file in 17 s
(`f04-opened.jpg`), a PNG opens in Firefox, kioclient exits 0.

### 2. Copy and paste of files (fixed: 9eb3786, 6eb09c0)

Two bugs in a row. (a) Every Ctrl+V first gave the guest the Mac's clipboard text unless it equalled the guest's
own last text; files copied in Dolphin are not text, so the paste pasted the Mac's old text (or nothing). The
server now counts every selection a guest program takes, and the page gives the host's text only when the
page has had the focus back since then. (b) The paste then reached KIO, which failed: "Cannot copy file from
/root/audit/notes.txt to /root/audit/sub/notes.txt. (Errno: 38)" (`k03-pasted.jpg`); RV_STRACE showed
`copy_file_range` = -38: rv answered ENOSYS, which cp takes as a fallback signal and KIO does not. rv now does
copy_file_range with reads and writes (native test: 256004 bytes identical, offsets right; busybox 62/62, debian
40/40). The end-to-end Dolphin paste with the new rv was not seen in the browser (see "Not verified").

### 3. Windows a third of the screen at scale 2 (fixed: 7539765)

wl_output.mode was sent in CSS pixels, the same as xdg_output's logical size; Qt then distrusts xdg_output
(its GNOME workaround in QWaylandScreen::geometry) and divides the mode by the scale: a 712x400 screen, and
new windows at most two thirds of that. Mode is now in the output's own pixels. Node-machine at 1424x800 scale
2: Kate 474x266 -> 854x533, Dolphin 474x266 -> 760x533; in the browser Dolphin 760x550, Settings 930x650
(`a05-dolphin.jpg` before, `f03-dolphin.jpg` after).

### 4. Files to the Mac (fixed: 5934b92)

`rv-host-save FILE` and Dolphin's context menu "Save to This Computer" (`f06-menu.jpg`): the file becomes a
download of the browser, at once after a click on the screen, else offered in the corner ("Save to this
computer: notes.txt 32 bytes", `f08-save-prompt.jpg`). Also a "Files to machine" button on the bar (a file
picker; the same as dropping files on the screen). Seen: gradient.png (101714 bytes) "saved at once"; the corner
prompt for a shell call. Whether headless Chrome wrote the download to disk was not checked.

### 5. Japanese, Chinese, Korean input (fixed: f3f9547)

There was no input method in the guest and the page sent only physical keys, so text no key makes could not
be typed (an `Input.insertText` of 日本 into Kate did nothing). The server now offers zwp_text_input_v3 (Qt and
GTK use it by themselves) and tells the page where the focused text field's cursor is; the page moves the
keyboard to a field of its own there, the host's input method composes in it, and the committed text goes to
the window. Seen with composition events: `echo 日本語 done` in Konsole (`k04-ime-konsole.jpg`), 中文测试 in Kate
(`k05-ime-kate.jpg`), 한국어 in a Firefox input field. Not tried with a person's real Mac input method.

### 6. Trackpad scrolling (fixed: e5281f9)

The runtime made one wheel click (three lines) per wheel event. The page now adds up deltaY, a click per 50
pixels, the first event of a gesture always one. Twenty 10-pixel events: 2 clicks (were 20).

### 7. Drag and drop between windows (open)

Dragging a file from one Dolphin to another (side by side, hovering 6 s over the target) shows the drag icon
following the pointer (`k09-dragging.jpg`), but wlserver.log says `drag: over (let go where nothing took it)`:
the target never sent wl_data_offer.accept with a type (or set_actions). Not found yet; a node-machine run with
logging on accept/set_actions/enter was started but the Mac was too loaded (load average 30) for it to get
Dolphin up in 1000 s. Next step: that run on a quieter machine (wl-dump's `slowdrag:` does the drag).

### 8. Window and program keys (fixed: 2e3bc5e)

Meta+Right reached the guest as Super+Right (Konsole printed a stray "C"). Now Meta+Left/Right/Up/Down tile the
active window (again: back), Meta+PgUp/PgDown maximize/minimize, Ctrl+Alt+T Konsole (4 s), Meta+E Dolphin (6.7 s), Alt+F2
and Alt+Space the launcher. Seen: all but Alt+Space.

### 9-20 (open)

- 9: `desktop wayland gpicview gradient.png` died with `rv: segmentation fault: address 0x354000005a0 at pc
  0x3dd2fd0` (GTK 2, through Xwayland). Images go to Firefox (30 s cold). Gwenview in the disk image, or the rv
  crash found (the rvfix/chromestart agents' area), would do.
- 11: Dolphin's Ctrl+L then a typed path (keys 150 ms later) gave "/root/root/audit" (the field had not
  selected its text yet); F2 then a name left the name unchanged. The keys arrive in order; the programs act on
  the shortcut late. Speed, mostly.
- 12: Kickoff has no Leave buttons; nothing ends the session cleanly (the page tab is closed instead).
- 13: `b05-htop.jpg`: VIRT 16.0E, RES 16.0E, MEM% 100 for every process under rv (NOMMU statm); the kernel's.
- 14, 16, 19: the disk image's (the orchestrator's): a user account, Firefox policies (no first-run pages),
  okular/gwenview/libnotify-bin.
- 15: no sound server or ALSA card: video and system sounds are silent.

## Not verified / caveats

- The Dolphin paste end to end with the copy_file_range fix in the browser (the fix was verified natively; the
  last browser session that was to show it ran out while the Mac slept).
- Download files actually landing in the Mac's Downloads folder (the page started the download).
- The IME with a real Mac input method and its candidate window (DevTools composition events only).
- All in headless Chrome for Testing; the owner's Chrome may treat focus, clipboard and downloads differently.
