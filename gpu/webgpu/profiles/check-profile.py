#!/usr/bin/env python3
"""The P4 oracle: does a device (vulkaninfo --json of wgvk) offer at least what the P4 profile has?

    check-profile.py [--allow NAME,NAME...] [--limits wgvk-limits.txt] p4-lavapipe.json VP_VULKANINFO_wgvk...json

P4 (profiles/p4-lavapipe.json) is lavapipe under the wgmask layer with spikes/wgmask/p4-env.sh: what zink's
GL 4.3 and Blender 4.3 ran on in spike S1. Checked:
  - every device extension of P4 is offered;
  - every feature that is true in P4 is true (by member name, over all feature structs: struct names differ
    between vulkaninfo versions);
  - every limit called max* is at least P4's, every min*Alignment at most P4's (elementwise for arrays);
    a limit named in --limits FILE (wgvk's honest WebGPU numbers) need only reach the floor given there.
Prints "ok NAME: ..." or one "FAIL NAME: ..." line per problem (at most 40), exit status 1 on failure.
"""
import json, sys


def dev(path):
    d = json.load(open(path))
    return list(d["capabilities"].values())[0]


def flat_features(c):
    out = {}
    for struct, fs in c.get("features", {}).items():
        for k, v in fs.items():
            if isinstance(v, bool):
                out[k] = out.get(k, False) or v
    return out


def limits(c):
    return c["properties"]["VkPhysicalDeviceProperties"]["limits"]


def main():
    args = sys.argv[1:]
    allow = set()
    floors = {}
    while args[0].startswith("--"):
        if args[0] == "--allow":
            allow = set(args[1].split(","))
        elif args[0] == "--limits":
            for line in open(args[1]):
                f = line.split()
                if f and not f[0].startswith("#"):
                    floors[f[0]] = float(f[1])
        args = args[2:]
    want, have = dev(args[0]), dev(args[1])
    fails = []
    exts = set(have.get("extensions", {}))
    for e in sorted(want.get("extensions", {})):
        if e not in exts and e not in allow:
            fails.append("extension %s missing" % e)
    fw, fh = flat_features(want), flat_features(have)
    unlisted = 0
    for k, v in sorted(fw.items()):
        if v and k not in fh:
            unlisted += 1	# (this vulkaninfo does not print that struct)
        elif v and not fh[k] and k not in allow:
            fails.append("feature %s is false" % k)
    lw, lh = limits(want), limits(have)
    nlim = 0
    for k, v in sorted(lw.items()):
        if k not in lh:
            fails.append("limit %s missing" % k)
            continue
        h = lh[k]
        if k in floors:
            if isinstance(h, (int, float)) and h < floors[k]:
                fails.append("limit %s = %s, below its floor %s (wgvk-limits.txt)" % (k, h, floors[k]))
            nlim += 1
            continue
        pairs = list(zip(v, h)) if isinstance(v, list) else [(v, h)]
        for a, b in pairs:
            if not isinstance(a, (int, float)) or isinstance(a, bool):
                continue
            if k.startswith("max") and b < a:
                fails.append("limit %s = %s, P4 has %s" % (k, h, v))
                break
            if k.startswith("min") and k.endswith("Alignment") and b > a:
                fails.append("limit %s = %s, P4 has %s" % (k, h, v))
                break
        nlim += 1
    name = have["properties"]["VkPhysicalDeviceProperties"].get("deviceName", "?")
    if fails:
        for f in fails[:40]:
            print("FAIL p4-profile: %s" % f)
        if len(fails) > 40:
            print("FAIL p4-profile: ... %d more" % (len(fails) - 40))
        sys.exit(1)
    print("ok p4-profile: %s offers P4's %d extensions (allowed missing: %s), %d true features (%d not printed by "
          "this vulkaninfo) and %d limits (%d of them at WebGPU's own value, above their floors)" % (name, len(want.get("extensions", {})), ",".join(sorted(allow)) or "none",
                                              sum(fw.values()), unlisted, nlim, len([k for k in floors if k in lh])))


main()
