// Text from an input method: zwp_text_input_v3 (text-input-unstable-v3.xml), for the text no key makes.
//
// The keyboard sends keys, and the clients make characters of them with the keymap; Japanese, Chinese or
// Korean text comes from an input method instead, which composes it from several keys and a choice. Here the
// input method is the host's own (the Mac's Japanese input, say): the page composes the text in a field of
// its own at the window's text cursor and hands over what was committed (table.js: imeText), and this sends
// it to the window with the keyboard as text_input's commit_string. Qt (QtWaylandClient's text-input-v3) and
// GTK (its "wayland" input module) both speak it, on their own, when the compositor offers it.
//
// What the page is told (table.js: ime): whether the window with the keyboard has a text input enabled now,
// and where its text cursor is on the screen (set_cursor_rectangle), so that the page puts its composing
// field, and with it the host's list of choices, there. Preedit (the text being composed) is shown by the
// page, not sent: a client that commits on focus changes would otherwise keep half-composed text.
(function (ws) {
  "use strict";

  const { send, log, table, H, global } = ws;
  const decoder = new TextDecoder("utf-8");
  const inputs = new Set();	// every zwp_text_input_v3
  let focus = null;		// the surface with the keyboard

  const clientOf = (surface) => surface.object.client;

  // The text input that takes text now: one of the keyboard's client's, entered on the focused surface, enabled.
  const active = () => {
    if (!focus || focus.destroyed) {
      return null;
    }
    for (const ti of inputs) {
      if (ti.client === clientOf(focus) && ti.entered === focus && ti.current.enabled) {
        return ti;
      }
    }
    return null;
  };

  let shown = "";
  const tell = () => {
    const ti = active();
    if (!ti) {
      if (shown !== "0") {
        shown = "0";
        table.ime(0);
      }
      return;
    }
    const [sx, sy] = ws.screenPos(ti.entered);
    const r = ti.current.rect || [0, 0, 0, 20];
    const now = [1, Math.round(sx + r[0]), Math.round(sy + r[1]), Math.max(1, Math.round(r[3]))];
    if (now.join() !== shown) {
      shown = now.join();
      table.ime(...now);
    }
  };

  const enter = (ti, surface) => {
    if (ti.entered === surface) {
      return;
    }
    // (Not to a surface its client has unmapped: the keyboard leaves it because the client took its window
    // away, and is about to destroy it, or did; a leave naming it can then reach a client that has forgotten
    // it: Qt's "Got leave event for surface 0x0", when Kickoff's popup closes.)
    if (ti.entered && !ti.entered.destroyed && ti.entered.slot >= 0) {
      send(ti, "leave", ti.entered.object);
    }
    ti.entered = surface;
    if (surface) {
      send(ti, "enter", surface.object);
    }
  };

  // From seat.js: the keyboard went to `surface` (or nowhere).
  ws.textInputFocus = (surface) => {
    focus = surface;
    for (const ti of inputs) {
      enter(ti, surface && ti.client === clientOf(surface) ? surface : null);
    }
    tell();
  };

  // Every turn (server.js): text the page committed goes to the active text input.
  ws.textInputTurn = () => {
    const bytes = table.imeText();
    if (bytes) {
      const ti = active();
      const text = decoder.decode(bytes);
      if (ti && text) {
        send(ti, "commit_string", text);
        send(ti, "done", ti.commits);
      } else if (ws.debug) {
        log("textinput: " + bytes.length + " bytes of text with no text input to take them");
      }
    }
    if (inputs.size) {
      tell();	// (the window may have moved: the cursor with it)
    }
  };

  H.zwp_text_input_manager_v3 = {
    destroy() {},
    get_text_input(manager, id, seat) {
      const ti = ws.create(manager.client, id, "zwp_text_input_v3", manager.version);
      ti.client = manager.client;
      ti.pending = { enabled: false, rect: null };
      ti.current = { enabled: false, rect: null };
      ti.commits = 0;
      ti.entered = null;
      inputs.add(ti);
      ti.onDestroy = () => {
        inputs.delete(ti);
        tell();
      };
      if (focus && !focus.destroyed && clientOf(focus) === manager.client) {
        enter(ti, focus);
      }
    },
  };

  H.zwp_text_input_v3 = {
    destroy() {},
    enable(ti) {
      ti.pending = { enabled: true, rect: null };	// (enable resets the state: text-input-unstable-v3.xml)
    },
    disable(ti) {
      ti.pending = { enabled: false, rect: null };
    },
    set_surrounding_text() {},
    set_text_change_cause() {},
    set_content_type() {},
    set_cursor_rectangle(ti, x, y, w, h) {
      ti.pending.rect = [x, y, w, h];
    },
    commit(ti) {
      ti.commits++;
      ti.current = { enabled: ti.pending.enabled, rect: ti.pending.rect || (ti.pending.enabled ? ti.current.rect : null) };
      tell();
    },
  };

  global("zwp_text_input_manager_v3", 1, () => {});
})
