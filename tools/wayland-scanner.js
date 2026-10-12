#!/usr/bin/env node
// The Wayland protocol, from its XML, as tables for the window server
// (wlserver/): for every interface, its requests and events with the types
// of their arguments, and its enums. wlserver/wire.js reads and writes
// messages by these tables alone, so a protocol is added by naming its XML
// here and running this again.
//
//   node tools/wayland-scanner.js wlserver/protocol/*.xml > wlserver/protocol.js
//
// The output is an expression for a function that returns the tables (the
// form every part of wlserver has: boot.js loads them with eval), of this
// shape:
//
//   { wl_surface: { version: 6,
//                   requests: [{ name: "attach", sig: "?oii", args: [["buffer", "?o", "wl_buffer"], ...],
//                                since: 1, destructor: false }, ...],
//                   events: [...],
//                   enums: { error: { invalid_scale: 0, ... } } }, ... }
//
// A signature has one letter per argument, in libwayland's spelling: i int,
// u uint, f fixed (24.8), s string, o object, n new_id, a array, h a file
// descriptor (sent beside the message, not in it); "?" before a letter means
// the argument may be null. A new_id without an interface (wl_registry.bind)
// is sent as the interface's name, its version and the id: "n" with no
// interface in args says so.
//
// The XML is read with a small matcher of its own: the protocol files use a
// handful of elements and no entities beyond the five, so a parser is not
// needed, and nothing has to be installed where this runs.
"use strict";

const fs = require("fs");

const files = process.argv.slice(2);
if (!files.length) {
  console.error("usage: wayland-scanner.js PROTOCOL.xml... > protocol.js");
  process.exit(2);
}

const unescape = (s) => s.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, "\"")
  .replace(/&apos;/g, "'").replace(/&amp;/g, "&");

// The attributes of a tag's text, as an object.
const attributes = (tag) => {
  const out = {};
  for (const m of tag.matchAll(/([a-zA-Z_:-]+)="([^"]*)"/g)) {
    out[m[1]] = unescape(m[2]);
  }
  return out;
};

// Every <name ...>...</name> (or <name .../>) element directly in `text`, with its attributes and inner text.
// Elements of the same name do not nest in these files (an <interface> has no <interface> inside).
const elements = (text, name) => {
  const out = [];
  const re = new RegExp("<" + name + "(\\s[^>]*?)?(/?)>", "g");
  let m;
  while ((m = re.exec(text)) !== null) {
    const attrs = attributes(m[1] || "");
    if (m[2] === "/") {
      out.push({ attrs, inner: "" });
      continue;
    }
    const end = text.indexOf("</" + name + ">", re.lastIndex);
    if (end < 0) {
      throw new Error("unterminated <" + name + ">");
    }
    out.push({ attrs, inner: text.slice(re.lastIndex, end) });
    re.lastIndex = end;
  }
  return out;
};

const LETTER = { int: "i", uint: "u", fixed: "f", string: "s", object: "o", new_id: "n", array: "a", fd: "h" };

const message = (element) => {
  const a = element.attrs;
  const args = [];
  let sig = "";
  for (const arg of elements(element.inner, "arg")) {
    const letter = LETTER[arg.attrs.type];
    if (!letter) {
      throw new Error(a.name + ": argument type " + arg.attrs.type + "?");
    }
    const nullable = arg.attrs["allow-null"] === "true";
    sig += (nullable ? "?" : "") + letter;
    const entry = [arg.attrs.name, (nullable ? "?" : "") + letter];
    if (arg.attrs.interface) {
      entry.push(arg.attrs.interface);
    } else if (letter === "n" || letter === "o") {
      entry.push(null);
    }
    if (arg.attrs.enum) {
      entry.push(arg.attrs.enum);
    }
    args.push(entry);
  }
  return { name: a.name, sig, args, since: Number(a.since || 1), destructor: a.type === "destructor" };
};

const protocol = {};
const sources = [];
for (const file of files) {
  const text = fs.readFileSync(file, "utf8").replace(/<!--[\s\S]*?-->/g, "");
  const proto = elements(text, "protocol")[0];
  if (!proto) {
    throw new Error(file + ": no <protocol>");
  }
  sources.push(proto.attrs.name);
  // (<copyright> and <description> hold text that may look like anything: taken out before the matching.)
  // (The empty ones first: <description summary="..."/> has no end tag for the second pattern to stop at.)
  const body = proto.inner.replace(/<copyright>[\s\S]*?<\/copyright>/g, "").replace(/<description[^>]*\/>/g, "")
    .replace(/<description(\s[^>]*)?>[\s\S]*?<\/description>/g, "");
  for (const iface of elements(body, "interface")) {
    const name = iface.attrs.name;
    const enums = {};
    for (const e of elements(iface.inner, "enum")) {
      const values = {};
      for (const entry of elements(e.inner, "entry")) {
        values[entry.attrs.name] = Number(entry.attrs.value);
      }
      enums[e.attrs.name] = values;
    }
    protocol[name] = {
      version: Number(iface.attrs.version),
      requests: elements(iface.inner, "request").map(message),
      events: elements(iface.inner, "event").map(message),
      enums,
    };
  }
}

// The tables as JavaScript, one message per line.
const lines = [];
lines.push("// The Wayland protocol as tables: generated by tools/wayland-scanner.js from " + sources.join(", ") + ".");
lines.push("// Do not edit: run the scanner again (its header says how, and what the tables mean).");
lines.push("(function () {");
lines.push("  \"use strict\";");
lines.push("  return {");
for (const name of Object.keys(protocol)) {
  const p = protocol[name];
  lines.push("    " + name + ": {");
  lines.push("      version: " + p.version + ",");
  for (const kind of ["requests", "events"]) {
    lines.push("      " + kind + ": [");
    for (const m of p[kind]) {
      lines.push("        " + JSON.stringify(m) + ",");
    }
    lines.push("      ],");
  }
  lines.push("      enums: " + JSON.stringify(p.enums) + ",");
  lines.push("    },");
}
lines.push("  };");
lines.push("})");
process.stdout.write(lines.join("\n") + "\n");
