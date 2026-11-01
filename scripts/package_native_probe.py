#!/usr/bin/env python3
"""Build a local, opt-in native probe package; never installs or boots it."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/probe"
REFERENCE = ROOT / "out/switch/boot-reference"
PINS = {
    "bl31.bin": (45655, "dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4"),
    "bl33.bin": (390884, "b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa"),
}
PROBE_SHA = "0cf22b9ebc368a7084e3b9346accf2692df7659e773eedaa2c6a38b08088ff22"
CACHE_HELPERS = {
    0x1920: bytes.fromhex("1f7108d5df3f03d5c0035fd6"),
    0x1A64: bytes.fromhex("9f3f03d5df3f03d5c0035fd6"),
}


def record(data: bytes) -> dict:
    return {"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def legacy_image(data: bytes, *, kind: int, name: str, load: int = 0,
                 entry: int = 0) -> bytes:
    # mkimage legacy format: U-Boot OS=17, ARM64=22, no compression=0.
    header = struct.pack(">7I4B32s", 0x27051956, 0, 0, len(data), load, entry,
                         zlib.crc32(data), 17, 22, kind, 0,
                         name.encode("ascii").ljust(32, b"\0"))
    header = header[:4] + struct.pack(">I", zlib.crc32(header)) + header[8:]
    return header + data


def validate_legacy(image: bytes, kind: int) -> bytes:
    if len(image) < 64:
        raise ValueError("truncated legacy image")
    v = struct.unpack(">7I4B32s", image[:64])
    header = image[:4] + bytes(4) + image[8:64]
    if (v[0] != 0x27051956 or v[1] != zlib.crc32(header)
            or v[3] != len(image) - 64 or v[6] != zlib.crc32(image[64:])
            or v[7:11] != (17, 22, kind, 0)):
        raise ValueError("invalid legacy image identity or checksum")
    return image[64:]


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for prefix in (Path("/opt/homebrew/opt/llvm/bin"), Path("/opt/homebrew/opt/lld/bin")):
        candidate = prefix / name
        if candidate.is_file():
            return str(candidate)
    raise ValueError(f"missing build tool: {name}")


def safe_output(path: Path) -> Path:
    path = path.absolute()
    allowed = ROOT / "out"
    if path.exists() or any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError("output must be a new directory with no symlink ancestry")
    if not path.resolve().is_relative_to(allowed.resolve()) or path.resolve() == allowed.resolve():
        raise ValueError("output must be an isolated directory below the workspace out directory")
    return path


def build(output: Path) -> dict:
    output = safe_output(output)
    firmware = {}
    for name, (size, sha) in PINS.items():
        path = REFERENCE / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"firmware source is not a regular file: {name}")
        data = path.read_bytes()
        if record(data) != {"size_bytes": size, "sha256": sha}:
            raise ValueError(f"pinned firmware identity mismatch: {name}")
        firmware[name] = data
    for offset, code in CACHE_HELPERS.items():
        if firmware["bl33.bin"][offset:offset + len(code)] != code:
            raise ValueError("BL33 cache helper does not match reviewed instructions")

    tools = {name: tool(name) for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-readelf", "llvm-objdump")}
    with tempfile.TemporaryDirectory(prefix="native-probe-build-", dir=ROOT / "out") as scratch:
        scratch = Path(scratch)
        commands = [
            [tools["clang"], "--target=aarch64-none-elf", "-march=armv8-a", "-c", str(SOURCE / "registers.S"), "-o", str(scratch / "probe.o")],
            [tools["ld.lld"], "-T", str(SOURCE / "link.ld"), "-o", str(scratch / "probe.elf"), str(scratch / "probe.o")],
            [tools["llvm-objcopy"], "-O", "binary", str(scratch / "probe.elf"), str(scratch / "probe.bin")],
        ]
        for command in commands:
            subprocess.run(command, check=True, capture_output=True, text=True)
        probe = (scratch / "probe.bin").read_bytes()
        if len(probe) != 768 or hashlib.sha256(probe).hexdigest() != PROBE_SHA:
            raise ValueError("compiled probe differs from the reviewed 768-byte instruction image")
        elf_report = subprocess.run([tools["llvm-readelf"], "-h", "-r", "-s", str(scratch / "probe.elf")], check=True, capture_output=True, text=True).stdout
        if "There are no relocations in this file." not in elf_report:
            raise ValueError("probe has unreviewed relocations")
        disassembly = subprocess.run([tools["llvm-objdump"], "-d", str(scratch / "probe.elf")], check=True, capture_output=True, text=True).stdout

    image = legacy_image(probe, kind=1, name="IOSwitch register probe", load=0xA0000000, entry=0xA0000000)
    if validate_legacy(image, 1) != probe or len(image) != 832:
        raise ValueError("standalone wrapper validation failed")
    template = (SOURCE / "boot.cmd.in").read_text()
    if template.count("@IMAGE_CRC32@") != 1:
        raise ValueError("expected exactly one image CRC token")
    script = template.replace("@IMAGE_CRC32@", f"{zlib.crc32(image):08x}").encode("ascii")
    script_image = legacy_image(struct.pack(">II", len(script), 0) + script, kind=6, name="IOSwitch OLED probe script")
    decoded = validate_legacy(script_image, 6)
    if decoded != struct.pack(">II", len(script), 0) + script:
        raise ValueError("script wrapper validation failed")

    prefix = "switchroot/ioswitch-probe-v3/"
    artifacts = {prefix + name: data for name, data in firmware.items()}
    artifacts.update({
        prefix + "registers.bin": probe,
        prefix + "registers.uboot.bin": image,
        prefix + "boot.cmd": script,
        prefix + "boot.scr": script_image,
        "bootloader/ini/ioswitch-probe-v3.ini": (SOURCE / "ioswitch-probe.ini").read_bytes(),
    })
    if any(len(data) >= (1 << 32) for data in artifacts.values()):
        raise ValueError("artifact exceeds the FAT32 file limit")
    manifest = {
        "schema_version": 1, "profile": "ioswitch-native-register-probe", "probe_revision": 3,
        "status": "local-staging-awaiting-independent-review",
        "native_switch_execution_verified": False, "installs_or_reboots": False,
        "target": {"t210b01": 1, "sku": 3, "isa": "AArch64 ARMv8-A"},
        "firmware_reference": str(REFERENCE.relative_to(ROOT)),
        "uboot_source_reference": "722e2b86be9ab1ae073335585c5997093e05f4f2",
        "cache_helpers": {hex(0xAA000000 + o): c.hex() for o, c in CACHE_HELPERS.items()},
        "memory": {"image_staging": "0xA0100000", "max_file_read_bytes": 832,
                   "probe_load": "0xA0000000", "probe_size_bytes": 768, "result": "0xA0000200"},
        "image_size_validation": {"command": "fatsize", "required_file_bytes": 832,
                                  "read_length_is_not_metadata": True},
        "image_crc32": f"{zlib.crc32(image):08x}",
        "files": {path: record(data) for path, data in sorted(artifacts.items())},
        "sources": {str(path.relative_to(ROOT)): record(path.read_bytes()) for path in
                    (SOURCE / "registers.S", SOURCE / "link.ld", SOURCE / "boot.cmd.in", SOURCE / "ioswitch-probe.ini", Path(__file__))},
        "tools": {name: {"path": path, "version": subprocess.run([path, "--version"], check=True, capture_output=True, text=True).stdout.splitlines()[0]} for name, path in tools.items()},
        "limitations": ["Not executed on OLED", "Screen output depends on existing Hekate/U-Boot framebuffer initialization",
                        "Uses two exact pinned firmware instruction tails; runtime opcode mismatch holds without execution",
                        "Probe returns to U-Boot script, which holds with sleep; physical power-off is the recovery path",
                        "No Linux, Android, iPadOS, QEMU, hypervisor, SD write, eMMC write, saveenv or reset in the script"],
    }
    output.mkdir(parents=True, exist_ok=False)
    for path, data in artifacts.items():
        dest = output / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "build-readelf.txt").write_text(elf_report)
    (output / "build-disassembly.txt").write_text(disassembly)
    # Verify actual bytes written, rather than only the in-memory artifact list.
    for path, expected in manifest["files"].items():
        if record((output / path).read_bytes()) != expected:
            raise ValueError(f"staged readback mismatch: {path}")
    (output / "verification.json").write_text(json.dumps({"all_staged_files_rehashed": True,
        "legacy_header_and_data_crcs_verified": True, "probe_matches_reviewed_machine_code": True,
        "firmware_matches_installed_reference": True, "source_only_review_pending": True,
        "hardware_execution_verified": False}, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/native-probe-v3-fat32")
    args = parser.parse_args()
    try:
        result = build(args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"native probe packaging failed: {exc}\n")
    print(json.dumps({"output": str(args.output.absolute()), "status": result["status"],
                      "files": result["files"]}, indent=2))


if __name__ == "__main__":
    main()
