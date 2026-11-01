#!/usr/bin/env python3
"""Stage the read-only U-Boot memory/display inventory; never installs it."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct

from package_native_probe import PINS, REFERENCE, ROOT, legacy_image, record, safe_output, validate_legacy


def build(output: Path) -> dict:
    output = safe_output(output)
    source = ROOT / "native/inventory"
    prefix = "switchroot/ioswitch-memory/"
    files = {}
    for name, (size, sha) in PINS.items():
        path = REFERENCE / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("firmware reference is not a regular file")
        data = path.read_bytes()
        if record(data) != {"size_bytes": size, "sha256": sha}:
            raise ValueError("firmware reference hash mismatch")
        files[prefix + name] = data
    script = (source / "boot.cmd").read_bytes()
    if b"@CARVEOUT_READS@" in script:
        raise ValueError("register offsets are not reviewed yet")
    script.decode("ascii")
    payload = struct.pack(">II", len(script), 0) + script
    image = legacy_image(payload, kind=6, name="IOSwitch firmware memory check")
    if validate_legacy(image, 6) != payload:
        raise ValueError("legacy script validation failed")
    files[prefix + "boot.cmd"] = script
    files[prefix + "boot.scr"] = image
    files["bootloader/ini/ioswitch-memory.ini"] = (source / "ioswitch-memory.ini").read_bytes()
    manifest = {
        "schema_version": 1,
        "profile": "ioswitch-firmware-memory-inventory",
        "status": "local-staging-awaiting-independent-review",
        "pages": 3,
        "seconds_per_page": 20,
        "firmware_reference": str(REFERENCE.relative_to(ROOT)),
        "files": {name: record(data) for name, data in sorted(files.items())},
        "sources": {str(path.relative_to(ROOT)): record(path.read_bytes()) for path in
                    (source / "boot.cmd", source / "ioswitch-memory.ini", Path(__file__),
                     ROOT / "scripts/package_native_probe.py")},
        "runtime_storage_writes": False,
        "new_cpu_payload": False,
        "os_handoff": False,
        "automatic_reboot": False,
        "installed": False,
        "hardware_result": "pending",
        "limitations": [
            "This observes U-Boot before OS cleanup; no post-bootm state is measured",
            "Control-FDT memory can be a template, not a measured bank list",
            "Bank ends and carveouts do not account for every active firmware allocation",
            "Photographs must show each page; shell/source review is not hardware execution",
        ],
    }
    output.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        if record(path.read_bytes()) != manifest["files"][name]:
            raise ValueError("staged readback failed")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/native-memory-check")
    args = parser.parse_args()
    print(json.dumps(build(args.output), indent=2))
