#!/usr/bin/env python3
# Make epoxy/missing.c: a stub for each GL function vrend refers to that Emscripten's WebGL2 does not have
# (the undefined symbols of a first link, one per line in argv[2]), with the prototype from the headers.
# A stub logs the first time it is called and does nothing (returns 0).
import re, sys
inc, names_file, out = sys.argv[1], sys.argv[2], sys.argv[3]
heads = "".join(open(p).read() for p in (f"{inc}/GLES3/gl32.h", f"{inc}/GLES2/gl2ext.h", sys.argv[4]))
protos = {}
for m in re.finditer(r"^[ \t]*(?:GL_APICALL[ \t]+|GLAPI[ \t]+)?([\w \t\*]+?)[ \t]*(?:GL_APIENTRY|APIENTRY)?[ \t]+(gl[A-Z]\w*)[ \t]*\(([^;{}#]*?)\);", heads, re.M):
    ret, name, args = " ".join(m.group(1).replace("GL_APICALL", "").replace("GLAPI", "").split()), m.group(2), " ".join(m.group(3).split())
    if ret.startswith("typedef") or "(" in ret:
        continue
    protos.setdefault(name, (ret, args))
names = sorted(set(l.strip() for l in open(names_file) if l.strip()))
# GL ES 3.0's own functions (WebGL2 has them all): a suffixed name of one of them (glGenBuffersARB,
# glDepthRangefOES) is the same function, as epoxy would resolve it, so it is forwarded rather than stubbed.
core30 = set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", open(f"{inc}/GLES3/gl3.h").read()))
lines = [open(out).read().split("/* -- generated below")[0].rstrip() + "\n",
         "/* -- generated below by gen-missing.py: do not edit below this line -- */"]
for n in names:
    if n not in protos:
        print("no prototype for", n, file=sys.stderr)
        continue
    ret, args = protos[n]
    params = [] if args in ("", "void") else [a.strip() for a in args.split(",")]
    core = re.sub(r"(ARB|EXT|OES|KHR)$", "", n)
    if core != n and core in core30:
        names_ = [re.findall(r"(\w+)\s*(?:\[\w*\])?$", p)[0] for p in params]
        call = f"{core}({', '.join(names_)});"
        lines.append(f"{ret} {n}({args})\n{{\n\t{'' if ret == 'void' else 'return '}{call}\n}}")
        continue
    body = f'MISSING("{n}");' + ("" if ret == "void" else f" return ({ret})0;")
    lines.append(f"{ret} {n}({args})\n{{\n\t{''.join('(void)' + re.findall(r'(\w+)\s*(?:\[\w*\])?$', p)[0] + '; ' for p in params if re.findall(r'(\w+)\s*(?:\[\w*\])?$', p))}{body}\n}}")
open(out, "w").write("\n".join(lines) + "\n")
print(len(names), "stubs")
