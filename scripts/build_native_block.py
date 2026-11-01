#!/usr/bin/env python3
"""Compile the IOSwitch block bridge and qualify its original storage imports.

The compile recipe and 190-slot audit come from the explicit isolated tree.
No native device is accessed. Original code is not modified. Based on that
tree's build-arm-driver.py and build-native-virtio-bindings.py checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
COMPONENT = "NativeIOSwitchBlockDevice"


def build(tree):
    tree = tree.resolve()
    source = REPO / "native/storage/NativeIOSwitchBlock.cpp"
    out = tree / "build/native-ioswitch-block"
    out.mkdir(parents=True, exist_ok=True)
    recipe = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    record = json.loads(recipe.read_text().strip().rstrip(","))
    destination = out / (COMPONENT + ".o")
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1
    command.extend(["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-Wno-c99-designator", "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
        "-Wno-unnecessary-virtual-specifier", "-mgeneral-regs-only",
        "-I", str(tree / ".cache/xnu-work/osfmk"),
        "-I", str(tree / "build/storage-abi/include")])
    (out / "compile-command.json").write_text(json.dumps(command, indent=2) + "\n")
    result = subprocess.run(command, cwd=record["directory"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out / "compile.log").write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-8000:])
    obj = destination.read_bytes()
    magic, cpu, subtype, kind, count, _ = struct.unpack_from("<6I", obj)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit("Unexpected driver object architecture")
    offset, deployment = 32, None
    for _ in range(count):
        cmd, length = struct.unpack_from("<II", obj, offset)
        if cmd == 0x32:
            deployment = struct.unpack_from("<II", obj, offset + 8)
        offset += length
    if deployment != (2, 0x110000):
        raise SystemExit(f"Unexpected driver deployment {deployment}")

    sys.path.insert(0, str(tree / "kits/NativePrelink"))
    import native_prelink as np
    audit_path = tree / "build/storage-abi/interface-audit.json"
    audit = json.loads(audit_path.read_text())
    if not audit["live_native_4k"] or audit["method_slots"] != 190 or not audit["abstract_complete_destructors_trap"]:
        raise SystemExit("Original live storage ABI has not been qualified")
    if len(audit["methods"]) != 190 or [row["slot"] for row in audit["methods"]] != list(range(190)):
        raise SystemExit("Original storage ABI audit must enumerate all 190 slots in order")
    # Unlike the generic helper, require the exact candidate to be present.
    candidate = tree / "build/native-prelink/native-apfs-load-positive.macho"
    if hashlib.sha256(candidate.read_bytes()).hexdigest() != audit["candidate_sha256"]:
        raise SystemExit("Original storage audit candidate identity changed")
    image = np.macho(obj)
    offset = next(at for cmd, at, size in image["commands"] if cmd == 2)
    sy, count, st, _ = struct.unpack_from("<4I", obj, offset + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from("<IBBHQ", obj, sy + i * 16)
        symbols.append((obj[st + ix:obj.index(b"\0", st + ix)].decode(), value, kind))
    mangled = str(len(COMPONENT)) + COMPONENT
    vtable = next(value for name, value, kind in symbols if name == "__ZTV" + mangled)
    relocations = {}
    for segment in image["segments"]:
        for section in segment["sections"]:
            if section["va"] <= vtable < section["va"] + section["size"]:
                ro, rc = struct.unpack_from("<II", obj, section["command_offset"] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from("<II", obj, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section["va"] + address] = symbols[word & 0xffffff][0]
    compiled = [relocations.get(vtable + 16 + i * 8) for i in range(190)]
    if not all(compiled) or vtable + 16 + 190 * 8 in relocations or "___cxa_pure_virtual" in compiled:
        raise SystemExit("Concrete block class has missing/additional/pure virtual methods")
    names = list(dict.fromkeys(compiled + [r["reference_name"] for r in audit["methods"]]))
    demangled = subprocess.check_output(["xcrun", "c++filt"],
        input="\n".join(n[1:] for n in names) + "\n", text=True).splitlines()
    readable = dict(zip(names, demangled))
    overrides = 0
    for row, actual in zip(audit["methods"], compiled):
        expected = row["reference_name"]
        own = actual.startswith(("__ZN" + mangled, "__ZNK" + mangled))
        if expected == "___cxa_pure_virtual":
            if not own:
                raise SystemExit("A required transport operation is not implemented")
        elif actual != expected:
            if not own or (row["slot"] not in (0, 1) and
                readable[actual].split("::", 1)[1] != readable[expected].split("::", 1)[1]):
                raise SystemExit("Concrete subclass dispatch mismatch at slot " + str(row["slot"]))
        overrides += own
    possible = {r["reference_name"]: int(r["candidate_target"], 16)
                for r in audit["methods"] if not r["kernel_names"]}
    possible.update({
        "__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass": int(audit["original_protected_constructor"], 16),
        "__ZN20IOBlockStorageDeviceD2Ev": int(audit["original_base_destructor"], 16),
        "__ZN20IOBlockStorageDevice10gMetaClassE": int(audit["original_metaclass"], 16),
        "__ZTV20IOBlockStorageDevice": int(audit["vtable"], 16)})
    undefined = [name for name, value, kind in symbols if kind == 1 and "IOBlockStorageDevice" in name]
    if any(name not in possible for name in undefined):
        raise SystemExit("Unqualified original storage import")
    if any(re.search(r"IOBlockStorageDeviceD[01]Ev$", name) for name in undefined):
        raise SystemExit("Concrete driver must not invoke abstract complete/deleting destruction")
    bindings = {name: possible[name] for name in undefined}
    assembly = out / "original-bindings.S"
    assembly.write_text("".join(f".globl {name}\n.set {name}, {value:#x}\n"
                                for name, value in sorted(bindings.items())))
    subprocess.run(["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-Werror",
        "-Wno-incompatible-sysroot", "-c", assembly, "-o", out / "original-bindings.o"], check=True)
    archive = out / ("lib" + COMPONENT + ".a")
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", archive, destination, out / "original-bindings.o"], check=True)
    report = dict(original_candidate_sha256=audit["candidate_sha256"],
        storage_audit_sha256=hashlib.sha256(audit_path.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        interface_sha256=hashlib.sha256((source.parent / "block_backend.h").read_bytes()).hexdigest(),
        compiled_object_sha256=hashlib.sha256(obj).hexdigest(), virtual_methods_checked=190,
        concrete_overrides=overrides, unresolved_original_imports=0,
        bindings={name: hex(value) for name, value in bindings.items()},
        original_code_modified=False, actual_io_qualified=False)
    (out / "bindings.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: 190 concrete slots, {overrides} overrides, {len(bindings)} qualified original imports")
    print(archive)
    print("Native I/O and APFS integration are not established by this compile/ABI check.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, required=True)
    build(parser.parse_args().tree)
