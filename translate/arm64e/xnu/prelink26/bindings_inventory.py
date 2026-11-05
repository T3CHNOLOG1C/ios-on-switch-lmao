#!/usr/bin/env python3
"""Every symbol our drivers bind to the original (iOS 17) kernelcache, with its 17 address and owning kext."""
import json, os, re, sys, bisect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kc17
T = os.path.join(HERE, "../../../../out/native-a57/root-v1/tree")
FILES = ["build/native-switch-root/original-bindings.S", "build/native-ram-block/original-bindings.S",
         "build/sheng-platform-power/original-bindings.S", "build/native-legacy-display/original-bindings.S",
         "build/native-hid-input/original-bindings.S", "drivers/SwitchUSB/original-bindings.S",
         "drivers/SwitchUSB/xhci-original-bindings.S", "drivers/SwitchBrightness/original-bindings.S",
         "drivers/SwitchAccelStub/original-bindings.S", "scripts/data/native-aes-original-bindings.S"]
SLIDE17 = -0x1000000
d, kn, names, kexts, gots = kc17.load()
# 17 extents per kext
ext = []
for bid, (m, ks) in kexts.items():
    for s in m["segments"]:
        if s["size"] and s["name"] != "__LINKEDIT": ext.append((s["va"], s["va"] + s["size"], bid))
ext.sort(); st = [e[0] for e in ext]
def owner(a):
    i = bisect.bisect_right(st, a) - 1
    return ext[i][2] if i >= 0 and a < ext[i][1] else None
rows = []
for f in FILES:
    txt = open(os.path.join(T, f)).read()
    for name, val in re.findall(r"\.set\s+(\S+),\s*(0x[0-9a-fA-F]+)", txt):
        a = int(val, 16)
        a17 = (a - SLIDE17) & ((1 << 64) - 1)          # bindings carry the prelink slide
        o = owner(a17)
        ok = name in names.get(a17, set())
        rows.append({"file": f, "symbol": name, "addr17": hex(a17), "kext": o or ("kernel?" if a17 in kn else None),
                     "name_checks_in_17": ok})
json.dump(rows, open(os.path.join(HERE, "bindings17.json"), "w"), indent=0)
import collections
print(len(rows), "symbols;", collections.Counter(r["kext"] for r in rows).most_common(20))
print("names confirmed by the 17 symbol tables:", sum(r["name_checks_in_17"] for r in rows))
print(collections.Counter((r["file"].split("/")[-2]) for r in rows))
