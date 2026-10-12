#!/usr/bin/env python3
# Make epoxy/include/epoxy/gl_extra.h: the desktop GL names (enums, types, prototypes) that vrend refers to
# and the GL ES 3.2 headers lack, taken from Emscripten's desktop headers (GL/gl.h, GL/glext.h: Khronos').
# Each enum is guarded with #ifndef; prototypes are only for functions the ES headers do not declare.
import re, sys
inc = sys.argv[1]  # Emscripten's sysroot include directory
es = "".join(open(f"{inc}/{h}").read() for h in ("GLES3/gl32.h", "GLES2/gl2ext.h"))
es_funcs = set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", es))
es_types = set(re.findall(r"typedef[^;]*\b(GL\w+)\s*;", es))
out = ["/* Made by gen-gl-extra.py from Emscripten's GL/gl.h and GL/glext.h: do not edit. */",
       "#ifndef RV_EPOXY_GL_EXTRA_H", "#define RV_EPOXY_GL_EXTRA_H", "#include <stddef.h>", "#include <stdint.h>", "struct _cl_context; struct _cl_event;",
       # (types the generic pass below misses: indented, platform-dependent, or pointers to functions)
       "typedef unsigned int GLhandleARB;", "typedef char GLcharARB;",
       "typedef void (*GLDEBUGPROCARB)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);",
       "typedef void (*GLDEBUGPROCAMD)(GLuint, GLenum, GLenum, GLsizei, const GLchar *, void *);",
       "typedef void (*GLprogramcallbackMESA)(GLenum, void *);"]
seen_ty = {"GLhandleARB", "GLcharARB"}
seen_enum, seen_fn = set(), set()
for h in ("GL/gl.h", "GL/glext.h"):
    text = open(f"{inc}/{h}").read()
    for m in re.finditer(r"^typedef ([^;()]*?)\b(GL\w+)\s*;", text, re.M):
        if m.group(2) not in es_types and m.group(2) not in seen_ty:
            seen_ty.add(m.group(2))
            out.append(f"typedef {m.group(1).strip()} {m.group(2)};")
    for m in re.finditer(r"^#define (GL_\w+)\s+(0x[0-9A-Fa-f]+u?|\d+u?)\s*$", text, re.M):
        name = m.group(1)
        if name not in seen_enum:
            seen_enum.add(name)
            out.append(f"#ifndef {name}\n#define {name} {m.group(2)}\n#endif")
    for m in re.finditer(r"^(?:GLAPI|WINGDIAPI)\s+(.+?)\s*(?:APIENTRY|GLAPIENTRY)\s+(gl[A-Z]\w*)\s*\((.*?)\);", text, re.M | re.S):
        ret, name, args = m.group(1), m.group(2), " ".join(m.group(3).split())
        if name in es_funcs or name in seen_fn:
            continue
        seen_fn.add(name)
        out.append(f"{ret.replace('GLAPI','').strip()} {name} ({args});")
out.append("#endif")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
print(len(seen_enum), "enums,", len(seen_fn), "functions,", len(seen_ty), "types")
