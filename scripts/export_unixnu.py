#!/usr/bin/env python3
"""Export a prepared UniXNU QEMU iPadOS run for a Linux bring-up attempt.

No firmware is downloaded, built, decrypted, mounted or flashed here. The OS
input must be the byte-for-byte raw whole disk signed by the prepared kernel's
VerifiedRoot policy, not its compressed IPSW DMG. SystemOS is its raw APFS
container. Structural checks and hashes do not prove either policy matches.

Contract follows xVanTuring/UniXNU-qemu scripts/run-qemu.py and board.toml:
https://github.com/xVanTuring/UniXNU-qemu/blob/main/scripts/run-qemu.py
https://github.com/xVanTuring/UniXNU-qemu/blob/main/board.toml
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import tempfile
import tomllib


class ExportError(ValueError):
    """The supplied files do not satisfy the portable runtime contract."""


def parse_adt(blob: bytes) -> dict:
    """Read Apple's flattened device tree with bounds and duplicate checks."""
    if len(blob) > 16 * 1024 * 1024:
        raise ExportError("device tree exceeds 16 MiB")
    offset = 0

    def read(size: int) -> bytes:
        nonlocal offset
        if size < 0 or offset + size > len(blob):
            raise ExportError("truncated device tree")
        result = blob[offset:offset + size]
        offset += size
        return result

    def node(depth: int) -> dict:
        if depth > 64:
            raise ExportError("device tree nesting exceeds 64 levels")
        count, child_count = struct.unpack("<II", read(8))
        if count > 4096 or child_count > 4096:
            raise ExportError("unreasonable device tree node count")
        props = {}
        for _ in range(count):
            key_bytes, length = struct.unpack("<32sI", read(36))
            try:
                key = key_bytes.split(b"\0", 1)[0].decode("ascii")
            except UnicodeDecodeError as exc:
                raise ExportError("non-ASCII device tree property") from exc
            if not key or key in props:
                raise ExportError("empty or duplicate device tree property")
            props[key] = read(length)
            read(-length % 4)
        children = [node(depth + 1) for _ in range(child_count)]
        return {"properties": props, "children": children}

    result = node(0)
    if offset != len(blob):
        raise ExportError("trailing bytes after device tree")
    return result


def string_property(node: dict, key: str) -> str:
    try:
        return node["properties"][key].rstrip(b"\0").decode("utf-8")
    except (KeyError, UnicodeDecodeError) as exc:
        raise ExportError(f"missing or invalid ADT property {key}") from exc


def child(node: dict, name: str) -> dict:
    matches = [item for item in node["children"] if string_property(item, "name") == name]
    if len(matches) != 1:
        raise ExportError(f"expected exactly one ADT node {name}")
    return matches[0]


def number_property(node: dict, key: str, width: int) -> int:
    value = node["properties"].get(key)
    if not isinstance(value, bytes) or len(value) != width:
        raise ExportError(f"missing or invalid ADT number {key}")
    return int.from_bytes(value, "little")


def validate_adt(blob: bytes) -> dict:
    tree = parse_adt(blob)
    model = string_property(tree, "model")
    product = child(tree, "product")
    idiom = string_property(product, "artwork-device-idiom")
    if model != "iPad7,1" or idiom != "pad":
        raise ExportError("expected the UniXNU iPad7,1 / pad identity")
    chosen = child(tree, "chosen")
    if number_property(chosen, "dram-base", 8) != 0x40000000:
        raise ExportError("ADT DRAM base does not describe QEMU virt")
    if number_property(chosen, "dram-size", 8) != 6144 << 20:
        raise ExportError("ADT must describe the upstream 6144 MiB guest")
    if number_property(chosen, "darwinboot-memory-size", 8) != 4096 << 20:
        raise ExportError("ADT must describe the upstream 4096 MiB kernel window")
    cpus = child(tree, "cpus")
    if len(cpus["children"]) != 1:
        raise ExportError("portable profile currently requires one guest CPU")
    cpu = child(cpus, "cpu0")
    if string_property(cpu, "compatible") != "arm,cortex-a710":
        raise ExportError("payload was not prepared for the TCG Cortex-A710 profile")
    if number_property(cpu, "timebase-frequency", 4) != 62500000:
        raise ExportError("TCG requires a 62500000 Hz ADT timer; rebuild the HVF payload")
    return {"model": model, "product_name": string_property(product, "product-name"),
            "device_idiom": idiom, "source": "esp/EFI/UniXNU/Payloads/kernel.macho.adt"}


def regular_file(path: Path) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as exc:
        raise ExportError(f"missing input: {path}") from exc
    if not stat.S_ISREG(mode):
        raise ExportError(f"input must be a regular file, not a symlink or device: {path}")
    if path.stat().st_size == 0:
        raise ExportError(f"empty input: {path}")


def validate_disks(os_disk: Path, systemos_disk: Path) -> None:
    for path in (os_disk, systemos_disk):
        regular_file(path)
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 512))
            if stream.read(4) == b"koly":
                raise ExportError(f"{path}: UDIF DMG supplied; export its raw disk bytes first")
    with os_disk.open("rb") as stream:
        stream.seek(512)
        if stream.read(8) != b"EFI PART":
            raise ExportError("OS input must be the raw GPT whole disk, not a partition or DMG")
    with systemos_disk.open("rb") as stream:
        header = stream.read(48)
    if len(header) != 48 or header[32:36] != b"NXSB":
        raise ExportError("SystemOS input must be the raw APFS container")
    block_size, block_count = struct.unpack_from("<IQ", header, 36)
    if block_size != 4096 or block_count == 0 or block_size * block_count != systemos_disk.stat().st_size:
        raise ExportError("SystemOS APFS container size does not match its 4 KiB block geometry")


def config_file(base: Path, name: str) -> Path:
    if not isinstance(name, str) or "\\" in name:
        raise ExportError("invalid file path in UniXNU configuration")
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise ExportError("UniXNU configuration path escapes the ESP")
    path = base.joinpath(*relative.parts)
    regular_file(path)
    return path


def validate_esp(esp: Path) -> dict:
    if not esp.is_dir() or esp.is_symlink():
        raise ExportError("ESP must be a real directory")
    for path in esp.rglob("*"):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ExportError(f"ESP contains a symlink or special file: {path}")
    regular_file(esp / "EFI/BOOT/BOOTAA64.EFI")
    base = esp / "EFI/UniXNU"
    config_path = base / "unixnu.toml"
    regular_file(config_path)
    try:
        config = tomllib.loads(config_path.read_text())
    except (tomllib.TOMLDecodeError, UnicodeError) as exc:
        raise ExportError("invalid unixnu.toml") from exc
    all_entries = config.get("entry", [])
    all_drivers = config.get("driver", [])
    if (not isinstance(all_entries, list) or not all(isinstance(entry, dict) for entry in all_entries)
            or not isinstance(all_drivers, list) or not all(isinstance(driver, dict) for driver in all_drivers)):
        raise ExportError("UniXNU entry and driver configuration must be arrays of tables")
    entries = [entry for entry in all_entries if entry.get("type") == "xnu"]
    if len(entries) != 1 or entries[0].get("path") != "Payloads/kernel.macho":
        raise ExportError("expected one prepared XNU kernel entry")
    entry = entries[0]
    config_file(base, entry["path"])
    if not entry.get("initrd"):
        raise ExportError("iPadOS payload has no prepared Data ramdisk")
    config_file(base, entry["initrd"])
    drivers = [driver.get("path", "") for driver in all_drivers]
    if "Payloads/DarwinBoot.efi" not in drivers or "Payloads/QemuRamfb.efi" not in drivers:
        raise ExportError("export a staged run ESP containing DarwinBoot and QemuRamfb drivers")
    for driver in drivers:
        config_file(base, driver)
    for name in ("kernel.macho.adt", "kernel.macho.trustcache"):
        regular_file(base / "Payloads" / name)
    if not isinstance(entry.get("args", ""), str):
        raise ExportError("XNU entry args must be a string")
    if (base / "Payloads/kernel.macho.gpuaddon").exists() or "sheng-gpu-addon=1" in entry.get("args", ""):
        raise ExportError("Mac Metal payload detected; prepare the upstream CPU renderer payload for Linux")
    return validate_adt((base / "Payloads/kernel.macho.adt").read_bytes())


def file_record(path: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return {"sha256": digest.hexdigest(), "size_bytes": path.stat().st_size}


def export_bundle(*, esp: Path, firmware: Path, os_disk: Path, systemos_disk: Path,
                  output: Path, source_revision: str, screen: str = "1376x1024") -> dict:
    """Stage a new bundle before publishing; never replace an existing destination."""
    if not re.fullmatch(r"[0-9a-fA-F]{40}", source_revision):
        raise ExportError("source revision must be the 40-character UniXNU-qemu commit SHA")
    dimensions = re.fullmatch(r"([1-9][0-9]{2,3})x([1-9][0-9]{2,3})", screen)
    if (not dimensions or int(dimensions[1]) % 16
            or not (320 <= int(dimensions[1]) <= 4096 and 320 <= int(dimensions[2]) <= 4096)):
        raise ExportError("screen must be WIDTHxHEIGHT within 320..4096, with width a multiple of 16")
    if output.exists() or output.is_symlink():
        raise ExportError("output already exists; choose a new bundle directory")
    if output.resolve().is_relative_to(esp.resolve()):
        raise ExportError("output must not be inside the source ESP")
    identity = validate_esp(esp)
    regular_file(firmware)
    validate_disks(os_disk, systemos_disk)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".ioswitch-export-", dir=output.parent))
    try:
        shutil.copytree(esp, temporary / "esp")
        (temporary / "firmware").mkdir()
        (temporary / "disks").mkdir()
        shutil.copyfile(firmware, temporary / "firmware/edk2-aarch64-code.fd")
        shutil.copyfile(os_disk, temporary / "disks/os.raw")
        shutil.copyfile(systemos_disk, temporary / "disks/systemos.raw")
        # A prepared build may change while files are copied. Only claim the
        # structural checks for the staged bytes that will actually be hashed.
        identity = validate_esp(temporary / "esp")
        validate_disks(temporary / "disks/os.raw", temporary / "disks/systemos.raw")
        manifest = {
            "schema_version": 1, "profile": "unixnu-ipados", "ram_mib": 6144,
            "cpu": "cortex-a710", "accel": "tcg", "smp": 1, "screen": screen,
            "firmware": "firmware/edk2-aarch64-code.fd", "esp": "esp",
            "os_disk": "disks/os.raw", "systemos_disk": "disks/systemos.raw",
            "identity": identity,
            "upstream": {"repository": "https://github.com/xVanTuring/UniXNU-qemu",
                         "revision": source_revision.lower(), "revision_source": "user-supplied",
                         "runtime_source": "scripts/run-qemu.py", "board_source": "board.toml"},
            "validation": {"structural_checks": "passed", "root_policy_match": "not verified",
                           "os_version": "not independently verified", "hardware_boot": "not tested"},
            "files": {str(path.relative_to(temporary)): file_record(path)
                      for path in sorted(temporary.rglob("*")) if path.is_file()},
        }
        (temporary / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # mkdir is the no-clobber gate even when another process creates output.
        output.mkdir()
        try:
            for path in temporary.iterdir():
                path.rename(output / path.name)
        except BaseException:
            shutil.rmtree(output)
            raise
        return manifest
    finally:
        shutil.rmtree(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp", type=Path, required=True, help="already staged upstream run/esp")
    parser.add_argument("--firmware", type=Path, required=True, help="matching QEMU EDK2 code image")
    parser.add_argument("--os-raw", type=Path, required=True, help="raw signed OS GPT whole disk")
    parser.add_argument("--systemos-raw", type=Path, required=True, help="raw SystemOS APFS container")
    parser.add_argument("--source-revision", required=True, help="prepared UniXNU-qemu checkout commit SHA")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--screen", default="1376x1024")
    args = parser.parse_args()
    try:
        manifest = export_bundle(esp=args.esp, firmware=args.firmware, os_disk=args.os_raw,
                                 systemos_disk=args.systemos_raw, output=args.output,
                                 source_revision=args.source_revision, screen=args.screen)
    except (ExportError, OSError) as exc:
        print(f"export failed: {exc}", file=sys.stderr)
        return 1
    print(f"Exported {len(manifest['files'])} files to {args.output}")
    print("Bundle integrity recorded. iPadOS boot and Switch compatibility remain unverified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
