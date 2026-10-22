import sys, os, glob, subprocess
# check-all.py [USR_DIR [LD_WASM]]: every wasm module under USR_DIR (default: sysroot-shared/usr; the nqs image's usr/ with
# build-nqs-image.sh) against ld-wasm and the libraries in USR_DIR/lib. Run in the build VM.
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/rvwasm/nativeqt/sysroot-shared/usr").rstrip("/")
mods = []
for dp, dn, fn in os.walk(root):
    for f in fn:
        p = os.path.join(dp, f)
        if os.path.islink(p) or not os.path.isfile(p) or p.endswith(".o"): continue
        try:
            with open(p, "rb") as h:
                if h.read(4) == b"\0asm": mods.append(p)
        except Exception: pass
libs = [m for m in mods if ".so" in os.path.basename(m) and os.path.dirname(m).endswith("usr/lib")]
prov = ["--provider", os.path.expanduser(sys.argv[2] if len(sys.argv) > 2 else "~/rvwasm/nativeqt/ref/ld-wasm")]
for l in libs: prov += ["--provider", l]
bad = 0
for m in sorted(mods):
    o = subprocess.run([os.path.join(here, "check-dylib.py"), m] + prov, capture_output=True, text=True)
    lines = [l for l in o.stdout.splitlines() if ("unresolved" in l and not l.rstrip().endswith(": 0")) or "FAIL" in l]
    if o.returncode or lines or o.stderr:
        bad += 1; print(m.replace(root, ""), lines[:3], o.stderr[-200:])
print("checked", len(mods), "wasm modules (", len(libs), "libraries in usr/lib ),", bad, "with problems")
