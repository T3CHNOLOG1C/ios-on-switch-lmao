"""tree_digest.py <dir>: one sha256 naming a directory's regular files (sorted relative path, NUL, file sha256, LF).

The gpu_host_inputs repository rule (bazel/gpu/deps.bzl) checks a host directory input (the m2v library store)
against its pin with this; run it by hand to compute a new pin."""
import hashlib
import os
import sys

root = sys.argv[1]
lines = []
for directory, dirs, files in os.walk(root, followlinks=True):
    dirs.sort()
    for name in sorted(files):
        if name == ".DS_Store":
            continue
        path = os.path.join(directory, name)
        with open(path, "rb") as f:
            lines.append(os.path.relpath(path, root) + "\0" + hashlib.sha256(f.read()).hexdigest() + "\n")
print(hashlib.sha256("".join(sorted(lines)).encode()).hexdigest(), len(lines))
