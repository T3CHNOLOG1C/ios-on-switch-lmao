#!/usr/bin/env python3
"""PC-relative references (B/BL, ADRP) from the selected kexts' __TEXT_EXEC that leave the kext."""
import sys, os, struct, bisect, collections, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcinfo import KC
kc = KC(sys.argv[1])
cl = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "closure.json")))
sel = set(cl["selected"])
ext = []
for n, vm, fo in kc.entries:
    for s in kc.entry_segs(fo):
        if s["size"] and s["name"] != "__LINKEDIT": ext.append((s["va"], s["va"] + s["size"], n, s["name"]))
ext.sort(); starts = [e[0] for e in ext]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return ext[i] if i >= 0 and a < ext[i][1] else None
def sx(v, b): return v - (1 << b) if v & (1 << (b - 1)) else v
kinds = collections.Counter(); bl_t = collections.Counter(); adrp_t = collections.Counter(); per = collections.Counter()
for n in sorted(sel):
    vm, fo = kc.entry[n]
    for s in kc.entry_segs(fo):
        if s["name"] != "__TEXT_EXEC": continue
        for sec in s["sections"]:
            if not sec["size"]: continue
            off = kc.va2off(sec["va"])
            words = struct.unpack_from("<%dI" % (sec["size"] // 4), kc.d, off)
            for i, w in enumerate(words):
                pc = sec["va"] + 4 * i
                if w & 0x7C000000 == 0x14000000:
                    t = pc + (sx(w & 0x3FFFFFF, 26) << 2); k = "BL" if w >> 31 else "B"
                elif w & 0x9F000000 == 0x90000000:
                    imm = sx(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21); t = (pc & ~4095) + (imm << 12); k = "ADRP"
                else:
                    continue
                o = owner(t if k != "ADRP" else t)
                if k == "ADRP":
                    # page granularity: inside if any byte of the page belongs to this kext
                    if any(e[2] == n and e[0] < t + 4096 and t < e[1] for e in ext if e[2] == n): continue
                    o = owner(t)
                    kinds[(k, o[2] if o and o[2] == "com.apple.kernel" else ("kext" if o else "none"))] += 1
                    if o and o[2] == "com.apple.kernel": adrp_t[t] += 1
                    continue
                if o and o[2] == n: continue
                dest = o[2] if o and o[2] == "com.apple.kernel" else ("sel-kext" if o and o[2] in sel else (o[2] if o else "none"))
                kinds[(k, dest)] += 1
                if dest == "com.apple.kernel": bl_t[t] += 1; per[n] += 1
for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:20]: print(k, v)
print("distinct kernel BL/B targets", len(bl_t), "ADRP kernel pages", len(adrp_t))
fx = set(cl["kernel_targets"])
print("BL targets also fixup targets:", len(set(bl_t) & fx), "union:", len(set(bl_t) | fx))
cl["kernel_branch_targets"] = sorted(bl_t); cl["kernel_adrp_pages"] = sorted(adrp_t)
json.dump(cl, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "closure.json"), "w"), indent=0)
for k, v in per.most_common(8): print("  ", k, v)
