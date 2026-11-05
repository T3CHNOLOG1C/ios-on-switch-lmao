#!/usr/bin/env python3
"""test_member.py <pins.json> <role> <size MiB> <out file> <out pins.json>: a stand-in IPSW member for low-space tests.

Writes a deterministic <size> MiB file and a copy of the pins whose <role> member has that file's size and sha256,
so the installer runs the same code path (copy, sha256 check against the pin) without the real 3.3 GB image.
Used by //install:test_system_cryptex and //install:test_ipsw_pins (--//install:system_cryptex=...)."""
import hashlib
import json
import sys
from pathlib import Path

pins_path, role, mib, out_file, out_pins = sys.argv[1:]
digest = hashlib.sha256()
with open(out_file, "wb") as f:
    for i in range(int(mib)):
        block = hashlib.sha256(b"SwitchTest member %d" % i).digest() * (1 << 15)
        f.write(block)
        digest.update(block)
pins = json.loads(Path(pins_path).read_text())
member = pins["members"][role]
member.update({"size": int(mib) << 20, "content_sha256": digest.hexdigest(), "test_stand_in": True})
Path(out_pins).write_text(json.dumps(pins, indent=2) + "\n")
