#!/usr/bin/env python3
# Spike S4: rebuild the objects of Mesa's shader compiler libraries from the NATIVE meson build's compile_commands.json
# with emcc (meson refuses an Emscripten cross build of Mesa: "ld.wasm does not support shared libraries"),
# then link spirv2nir.c against them. Run inside the native build directory.
import json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
libs = ["libvtn.a", "libnir.a", "libmesa_util.a", "libcompiler.a", "libmesa_util_c11", "libblake3"]
out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
cmds = json.load(open("compile_commands.json"))
jobs = []
# what the arm64 Linux build found that Emscripten does not have
UNDEF = {"-DUSE_AARCH64_ASM", "-DHAVE_ZLIB", "-DHAVE_COMPRESSION", "-DHAVE_SYS_INOTIFY_H", "-DHAVE_LINUX_UDMABUF_H",
         "-DHAVE_LINUX_FUTEX_H", "-DALLOW_KCMP", "-DHAVE_PTHREAD_SETAFFINITY", "-DHAS_SCHED_GETAFFINITY", "-DUSE_LIBELF",
         "-DHAVE_DL_ITERATE_PHDR", "-DHAVE_PROGRAM_INVOCATION_NAME", "-DHAVE_ISSIGNALING",
         "-DHAVE_STRTOD_L", "-DHAVE_RANDOM_R", "-DHAVE_UINT128", "-DHAVE_GETRANDOM", "-DHAVE_MEMFD_CREATE",
         "-DHAVE_SYS_SHM_H", "-DHAVE_FLOCK", "-DHAVE_POSIX_FALLOCATE", "-DHAVE_FUNC_ATTRIBUTE_ALIAS"}
drop = re.compile(r"^-(m(sse|avx|popcnt|f16c|fma|bmi|arch=|tune=|tls-dialect=).*|fstack-protector.*|fcf-protection.*|flifetime-dse.*|Werror.*)$")
for e in cmds:
    if not any(l in e["output"] for l in libs): continue
    a = shlex.split(e["command"])
    a[0] = "emcc" if a[0] in ("cc", "gcc") else "em++"
    a = [x for x in a if not drop.match(x) and x not in UNDEF]
    i = a.index("-o"); o = os.path.join(out, e["output"].replace("/", "_")); a[i + 1] = o
    for flag in ("-MD", "-MQ", "-MF"):
        while flag in a:
            j = a.index(flag); del a[j:j + (1 if flag == "-MD" else 2)]
    a += ["-pthread", "-Wno-error", "-D__linux__=0"] if False else ["-pthread", "-Wno-error"]
    jobs.append((e["file"], a, o))
def run(j):
    f, a, o = j
    r = subprocess.run(a, cwd=os.getcwd(), capture_output=True, text=True)
    return (f, r.returncode, r.stderr[-1500:])
bad = 0
with ThreadPoolExecutor(6) as ex:
    for f, rc, err in ex.map(run, jobs):
        if rc:
            bad += 1
            if bad <= 6: print("FAIL", f, err)
print("objects", len(jobs), "failed", bad)
