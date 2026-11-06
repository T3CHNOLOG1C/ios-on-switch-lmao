#!/usr/bin/env python3
"""ptrauth string discriminators (clang's __builtin_ptrauth_string_discriminator), computed by clang itself:
one generated C file, compiled for arm64e and run on this Mac. Cached in the work directory (disc-cache.json)."""
import json, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths
CACHE = paths.work("disc-cache.json")

def discriminators(names):
    cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}
    todo = sorted(set(n for n in names if n not in cache))
    if todo:
        with tempfile.TemporaryDirectory() as t:
            src = os.path.join(t, "d.c")
            with open(src, "w") as f:
                f.write("#include <stdio.h>\nint main(void){\n")
                for n in todo:
                    lit = n.replace("\\", "\\\\").replace('"', '\\"')
                    f.write('printf("%%u\\n", (unsigned)__builtin_ptrauth_string_discriminator("%s"));\n' % lit)
                f.write("return 0;}\n")
            subprocess.run(["xcrun", "clang", "-arch", "arm64e", "-O0", "-o", os.path.join(t, "d"), src], check=True)
            out = subprocess.run([os.path.join(t, "d")], capture_output=True, text=True, check=True).stdout.split()
        for n, v in zip(todo, out): cache[n] = int(v)
        json.dump(cache, open(CACHE, "w"))
    return {n: cache[n] for n in names}
