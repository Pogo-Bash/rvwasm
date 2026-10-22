# hostopen: web pages from the machine, opened in the host's browser

The machine's Firefox and Chromium are riscv64 programs under the emulator:
no JavaScript JIT, video decoded in software. A heavy site (video above all)
is slow there and can take more memory than the machine has. The browser the
page runs in does all of that natively. So web addresses from the machine can
be opened there, in a tab of the person's own browser:

    rv-host-open URL                 from a terminal in Debian (also "example.com")
    xdg-open URL, kde-open URL,      what programs run to open a link: an http or https address goes to the
    x-www-browser URL, $BROWSER      host; anything else (files, mailto:, ...) to Debian's program as before
    rv-host-open --install           make "Open in host browser" the default web browser of this user
                                     (KDE's settings, GTK, xdg-mime: ~/.config/mimeapps.list), and list it in
                                     the launchers; --uninstall undoes it
    rv-host-open --off / --on        everything opens in the machine again / on the host (the default)
    Alt-Shift-O, or "Page to host"   (the page, while the window server runs) the page of the Firefox or
                                     Chromium window in front, offered to the host (asks first, like every address)

## Files the other way: rv-host-save

    rv-host-save FILE...             each file a download of the host's browser (the person's Downloads
                                     folder), at most 512 MiB each; folders: pack them first
    Dolphin: a file's context menu   "Save to This Computer" (a service menu: tools/x/xdg/data/kio/servicemenus)

The machine's side (`boot.js --save FILE`) reads the file into its own memory and says where on the
channel, with a mark as below; the page copies it out of its machine's memory and hands it to the
browser as a download: at once if the person clicked on the screen in the last three seconds (choosing
the menu entry is such a click), otherwise asked for in the same box as an address ("Save **NAME** (size)
to this computer?", Save / Cancel; the box is described below). The other way round, files dropped on the screen, or picked with the
bar's "Files to machine", go into ~/Downloads in the machine (wlserver/README.md).

## How an address gets out

`bin/rv-host-open` runs `boot.js` with hostnode (a JavaScript process of the
machine, like the window server). It checks the address, writes a random mark
of 32 bytes into the machine's memory, and says `{ type: "open", id, url,
mark, markBytes }` on the BroadcastChannel `rv-host-ID` (ID: the page load's, `rvtab=ID` on the kernel's command line); then it waits up to
three seconds for `{ type: "open-answer", id, status }` and exits 0 (opened,
or shown to the person), 1 (refused) or 3 (no page answered: the wrappers
then use Debian's program instead).

The page's side is `tools/www/hostopen.js`. Every tab of the dev server's
origin hears the channel, and each tab may run a machine of its own: a page
takes a request only if the mark is in its own machine's memory at the place
the message names, so a link opened in one tab's machine never opens from
another tab. (This is why it is not a plain message: the window server's
table is found the same way, by being in the page's own machine's memory.)
It needs no window server: it works under X (Plasma, IceWM) as under
`desktop wayland`.

## What the page lets through

- http and https only; at most 2048 characters; no spaces or control
  characters; no name or password in the address (`https://bank.example@evil`
  style). `javascript:`, `data:`, `file:`, `blob:` and every other kind are
  refused: a guest program is not to run script in the page's origin, read
  the host's files or start the host's programs. Checked in `boot.js` (to say
  why at once) and again in the page (which is what opens it).
- **Nothing opens without the person's answer** (owner, 2026-10-14). Every address is shown in a box of the
  page's own, outside the machine's screen (a fixed box at the top of the page, over the bar's row, in the
  landing page's and the top bar's look: Catppuccin Mocha, JetBrains Mono): "Open **example.com**/watch... in a
  new tab?", the whole address under it with the host in colour (the host as the browser has it: an
  international name in its ASCII form, so look-alike letters show), and Open / Cancel. The click on Open is
  the user activation the browser wants for a new tab. (Until then a click on the screen in the last three
  seconds opened the tab at once; that shortcut is gone for addresses, and Page to host asks too.) Several
  waiting: the newest is shown first, with "N waiting"; they are forgotten after two minutes. Enter opens and
  Escape cancels while the box has the keyboard (click it, or Tab to it); it never takes the keys by itself,
  so a program in the machine cannot have the person's typing open a tab.
- The new tab has no opener (it cannot reach the page) and, from the link, no
  referrer.

## The default browser (owner, 2026-10-14)

Web links opened inside the machine, by any program (KDE's, native or emulated: Konsole's Ctrl+click,
KIO's OpenUrlJob in Dolphin and Okular, KNewStuff's "Open the homepage"; xdg-open, kde-open, $BROWSER), go to the
person's own browser as a new tab, after the box above, not to Debian's emulated browsers (which one KDE
picked used to depend on how its cache happened to be built: firefox-esr, chromium or luakit).
`tools/x/xdg` (the desktop's system defaults, on XDG_CONFIG_DIRS and XDG_DATA_DIRS after Debian's, and so after the user's own
~/.config) has `config/mimeapps.list` (`x-scheme-handler/http`, `x-scheme-handler/https` and `text/html` ->
`rv-host-open.desktop`) and `data/applications/rv-host-open.desktop`; the desktop script makes KDE's cache again once
(stamp `~/.cache/rv-ksycoca-xdg`) so that a home from before has it. A local file (`file:///x.html`, a path)
is not for the host, which cannot read it: `rv-host-open` hands it to the machine's own browser. Firefox ESR and
Chromium start from the menu as before. A person's own choice (`~/.config/mimeapps.list`, "Open with ... always")
wins over these defaults; `rv-host-open --install` writes it again, `--uninstall` removes it.

## Firefox and Chromium in the machine

Nothing turns a site into an address for the host on its own: a browser
offers no setting that sends chosen domains to another program. What there
is:

- **Page to host** (Alt-Shift-O, or the bar's button, while the window
  server runs): the page presses Ctrl-L and Ctrl-C in the window in front
  (Firefox, Chromium, Falkon, Konqueror, Epiphany, Midori by app id), takes
  the address from the clipboard bridge, presses Escape, and opens the
  address to the host's box (Open / Cancel, like every address). Costs: the address is left on the guest's and the host's
  clipboard, and the address bar keeps the keyboard. It needs the window
  server (under X there is no clipboard bridge).
  Seen in the browser: the bar's button with Firefox in front opened
  example.com on the host at once; Alt-Shift-O once found Firefox (busy
  just after starting) not copying within 5 s, so the page now waits 10 s.
- Links from other programs (Konsole, Kate, KDE's help, mail) go to the host
  through xdg-open, kde-open, $BROWSER, or the default browser after
  `rv-host-open --install`.
- Not done, and what it would take to route domains automatically: a small
  Firefox extension (webRequest on the top-level navigation to listed
  domains, cancelled and handed over by native messaging to rv-host-open),
  installed by `policies.json` (ExtensionSettings) in
  `/usr/lib/firefox-esr/distribution/` with `xpinstall.signatures.required`
  false (honoured by ESR) in `/etc/firefox-esr/firefox-esr.js`. Both files
  are in the disk image, which belongs to the image's builder; a Chromium
  version would need the same with its policies in `/etc/chromium/policies`.

## Guest servers at localhost

`tools/guest-port.py` (run in the build VM, next to rvRelay) makes
a server in the machine, `python3 -m http.server 8080` say, reachable from
the host's browser at `http://localhost:8080/`: it listens on the VM's
127.0.0.1 and passes connections to the machine (10.0.5.N), and Lima forwards
the VM's local ports to the Mac's. Its header says what it lets through: only
the ports named, only requests whose Host is localhost:PORT (against DNS
rebinding), nothing beyond localhost; the guest's servers get an origin of
their own, not the dev page's.

    tools/guest-port.py 8080                the guest's 8080 at localhost:8080
    tools/guest-port.py 18080:8080          ... at localhost:18080 (when the Mac's 8080 is taken)
