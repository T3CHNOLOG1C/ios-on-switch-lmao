#!/usr/bin/env python3
"""Build a local native OLED handoff diagnostic; never installs or boots it."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/handoff"
REFERENCE = ROOT / "out/switch/boot-reference"
DTB_PATH = ROOT / "out/research-native/handoff-inventory/oled-sku3.dtb"
PINS = {
    "bl31.bin": (45655, "dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4"),
    "bl33.bin": (390884, "b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa"),
    "oled-sku3.dtb": (166994, "a806b66975040d7a8ba7c04c4f173a657b610a90868eab9d0efa806c8350ce63"),
}
LOAD = 0xA0000000
MAX_RAW = 128 * 1024
PREFIX = "switchroot/ioswitch-handoff-v2/"
INI = b"[IOSwitch Native Handoff v2]\nl4t=1\nboot_prefixes=/switchroot/ioswitch-handoff-v2/\nid=IOSWHN2\n"
BUILD_FLAGS = ["--target=aarch64-none-elf", "-march=armv8-a", "-mstrict-align", "-mgeneral-regs-only",
               "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-O2",
               "-Wall", "-Wextra", "-Werror"]


def record(data: bytes) -> dict:
    return {"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def read_regular(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular input file: {path}")
    return path.read_bytes()


def pinned(path: Path, name: str) -> bytes:
    data = read_regular(path)
    size, sha = PINS[name]
    if record(data) != {"size_bytes": size, "sha256": sha}:
        raise ValueError(f"pinned input identity mismatch: {name}")
    return data


def legacy_image(data: bytes, *, os_type: int, kind: int, name: str,
                 load: int = 0, entry: int = 0) -> bytes:
    title = name.encode("ascii")
    if len(title) > 32:
        raise ValueError("legacy image name is too long")
    header = struct.pack(">7I4B32s", 0x27051956, 0, 0, len(data), load, entry,
                         zlib.crc32(data), os_type, 22, kind, 0, title.ljust(32, b"\0"))
    header = header[:4] + struct.pack(">I", zlib.crc32(header)) + header[8:]
    image = header + data
    decoded = struct.unpack(">7I4B32s", image[:64])
    zeroed = image[:4] + bytes(4) + image[8:64]
    if (decoded[0] != 0x27051956 or decoded[1] != zlib.crc32(zeroed)
            or decoded[3:7] != (len(data), load, entry, zlib.crc32(data))
            or decoded[7:11] != (os_type, 22, kind, 0)):
        raise ValueError("legacy wrapper validation failed")
    return image


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for prefix in (Path("/opt/homebrew/opt/llvm/bin"), Path("/opt/homebrew/opt/lld/bin")):
        path = prefix / name
        if path.is_file():
            return str(path)
    raise ValueError(f"missing build tool: {name}")


def safe_output(path: Path) -> Path:
    path = path.absolute()
    allowed = ROOT / "out"
    if path.exists() or any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError("output must be a new directory with no symlink ancestry")
    if not path.resolve().is_relative_to(allowed.resolve()) or path.resolve() == allowed.resolve():
        raise ValueError("output must be an isolated directory below workspace out")
    return path


def validate_elf(data: bytes) -> dict:
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected little-endian ELF64")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    etype, machine, version, entry, phoff = header[:5]
    phsize, phnum = header[8:10]
    if (etype, machine, version, entry) != (2, 183, 1, LOAD):
        raise ValueError("ELF must be an AArch64 executable with entry 0xA0000000")
    if phsize != 56 or not 1 <= phnum <= 32 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid ELF program header table")
    segments = []
    for index in range(phnum):
        kind, flags, offset, virt, phys, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + index * phsize)
        if kind != 1:
            continue
        if (virt != phys or not LOAD <= virt < LOAD + MAX_RAW or filesz > memsz
                or virt + memsz > LOAD + MAX_RAW or offset + filesz > len(data)):
            raise ValueError("ELF load segment escapes the 128KiB diagnostic region")
        segments.append({"address": hex(virt), "file_size_bytes": filesz,
                         "memory_size_bytes": memsz, "flags": flags, "alignment": align})
    if not segments or not any(int(s["address"], 16) == LOAD and s["flags"] & 1 for s in segments):
        raise ValueError("ELF entry is not covered by an executable load segment")
    return {"entry": hex(entry), "load_segments": segments,
            "footprint_end_exclusive": hex(max(int(s["address"], 16) + s["memory_size_bytes"] for s in segments))}


def build(output: Path, binary_path: Path | None = None, elf_path: Path | None = None) -> dict:
    output = safe_output(output)
    if (binary_path is None) != (elf_path is None):
        raise ValueError("--binary and --elf must be supplied together")
    sources = sorted(p for p in SOURCE.iterdir() if p.suffix in {".S", ".c", ".h", ".ld", ".in"})
    sources.append(Path(__file__))
    source_records = {str(path.relative_to(ROOT)): record(read_regular(path)) for path in sources}
    firmware = {name: pinned(REFERENCE / name, name) for name in ("bl31.bin", "bl33.bin")}
    dtb = pinned(DTB_PATH, "oled-sku3.dtb")
    if struct.unpack_from(">II", dtb) != (0xD00DFEED, len(dtb)):
        raise ValueError("OLED FDT header size does not match its pinned file")
    tools = {name: tool(name) for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-readelf", "llvm-objdump")}
    commands = []
    with tempfile.TemporaryDirectory(prefix="native-handoff-build-", dir=ROOT / "out") as scratch_name:
        scratch = Path(scratch_name)
        built_elf, built_bin = scratch / "handoff.elf", scratch / "handoff.bin"
        if binary_path is None:
            for source_name in ("entry.S", "probe.c"):
                source_file = SOURCE / source_name
                read_regular(source_file)
                obj = scratch / (source_file.stem + ".o")
                commands.append([tools["clang"], *BUILD_FLAGS, "-I", str(SOURCE), "-c",
                                 str(source_file), "-o", str(obj)])
            commands.append([tools["ld.lld"], "-T", str(SOURCE / "link.ld"),
                             "-o", str(built_elf), str(scratch / "entry.o"), str(scratch / "probe.o")])
            for command in commands:
                subprocess.run(command, check=True, capture_output=True, text=True)
        else:
            built_elf.write_bytes(read_regular(elf_path))
        elf = built_elf.read_bytes()
        elf_layout = validate_elf(elf)
        objcopy = [tools["llvm-objcopy"], "-O", "binary", str(built_elf), str(built_bin)]
        commands.append(objcopy)
        subprocess.run(objcopy, check=True, capture_output=True, text=True)
        raw = built_bin.read_bytes()
        if not 4 <= len(raw) <= MAX_RAW:
            raise ValueError("native diagnostic must fit within 128KiB")
        if binary_path is not None and raw != read_regular(binary_path):
            raise ValueError("supplied raw image does not match the supplied ELF")
        elf_report = subprocess.run([tools["llvm-readelf"], "-h", "-l", "-r", "-s", str(built_elf)],
                                    check=True, capture_output=True, text=True).stdout
        if "There are no relocations in this file." not in elf_report:
            raise ValueError("diagnostic ELF contains unreviewed relocations")
        disassembly = subprocess.run([tools["llvm-objdump"], "-d", str(built_elf)],
                                     check=True, capture_output=True, text=True).stdout

    # Linux OS tag selects this firmware's kernel handoff ABI. Payload is only
    # the locally authored diagnostic, not a Linux, XNU or iPadOS kernel.
    image = legacy_image(raw, os_type=5, kind=2, name="IOSwitch native handoff", load=LOAD, entry=LOAD)
    if len(image) > MAX_RAW + 64:
        raise ValueError("kernel wrapper exceeds its bounded staging region")
    template = read_regular(SOURCE / "boot.cmd.in").decode("ascii")
    substitutions = {"@IMAGE_BYTES_HEX@": hex(len(image)), "@IMAGE_CRC32@": f"{zlib.crc32(image):08x}",
                     "@DTB_BYTES_HEX@": hex(len(dtb)), "@DTB_CRC32@": f"{zlib.crc32(dtb):08x}"}
    for token, value in substitutions.items():
        if token not in template:
            raise ValueError(f"missing generated script token {token}")
        template = template.replace(token, value)
    if re.search(r"@[A-Z0-9_]+@", template):
        raise ValueError("unresolved generated script token")
    script = template.encode("ascii")
    script_data = struct.pack(">II", len(script), 0) + script
    script_image = legacy_image(script_data, os_type=17, kind=6, name="IOSwitch handoff script")
    artifacts = {PREFIX + name: data for name, data in firmware.items()}
    artifacts.update({PREFIX + "handoff.bin": raw, PREFIX + "handoff.uboot.bin": image,
                      PREFIX + "oled-sku3.dtb": dtb, PREFIX + "boot.cmd": script,
                      PREFIX + "boot.scr": script_image, "bootloader/ini/ioswitch-handoff-v2.ini": INI})
    if any(len(data) >= (1 << 32) for data in artifacts.values()):
        raise ValueError("artifact exceeds the FAT32 file limit")
    current_sources = sorted(p for p in SOURCE.iterdir() if p.suffix in {".S", ".c", ".h", ".ld", ".in"})
    current_sources.append(Path(__file__))
    current_records = {str(path.relative_to(ROOT)): record(read_regular(path)) for path in current_sources}
    if current_records != source_records:
        raise ValueError("source files changed while the diagnostic was being built; rebuild after edits finish")
    manifest = {
        "schema_version": 1, "profile": "ioswitch-native-handoff-probe", "probe_revision": 2,
        "status": "local-staging-awaiting-independent-review", "native_handoff_verified": False,
        "installs_or_reboots": False, "source_build": binary_path is None,
        "target": {"t210b01": 1, "sku": 3, "isa": "AArch64 ARMv8-A", "nonreturning": True},
        "uboot_source_reference": "722e2b86be9ab1ae073335585c5997093e05f4f2",
        "legacy_kernel_header": {"os": 5, "architecture": 22, "type": 2, "compression": 0,
                                 "load": hex(LOAD), "entry": hex(LOAD)},
        "memory": {"script": "0x8FE00000", "fdt": "0x8D000000", "fdt_window_bytes": 0x40000,
                   "fdt_initial_capacity_bytes": 0x30000, "fdt_bootm_pad_bytes": 0x3000,
                   "fdt_high": "ffffffffffffffff", "image_staging": "0xA0100000",
                   "image_staging_limit_bytes": MAX_RAW + 64, "native_load": hex(LOAD),
                   "native_limit_bytes": MAX_RAW, "framebuffer": "0xF5A00000",
                   "framebuffer_end_exclusive": "0xF5D84000", "framebuffer_reserved_bytes": 0x400000},
        "validation": {"file_size_source": "fatsize metadata", "image_exact_read_bytes": len(image),
                       "image_crc32": f"{zlib.crc32(image):08x}", "dtb_exact_read_bytes": len(dtb),
                       "dtb_crc32": f"{zlib.crc32(dtb):08x}", "elf": elf_layout,
                       "framebuffer_live_compared": ["address", "width", "height", "format", "rotation", "size_property_length"],
                       "framebuffer_size_value_live_compared": False,
                       "framebuffer_size_basis": "exact pinned BL33 control DTB plus OLED memory-inventory photos; current value printed only",
                       "fdt_get_value_size_quirk": "00384000 is misidentified as a printable string and becomes empty; get-addr is avoided"},
        "files": {path: record(data) for path, data in sorted(artifacts.items())},
        "sources": source_records,
        "build_commands": commands,
        "tools": {name: {"path": path, "version": subprocess.run([path, "--version"], check=True,
                  capture_output=True, text=True).stdout.splitlines()[0]} for name, path in tools.items()},
        "limitations": ["Not executed on OLED; independent source and machine-code review pending",
                        "bootm prep fixes up the copied FDT and queries PSCI_VERSION through SMC; preparation errors may hang inside U-Boot",
                        "bootm go quiesces devices and disables MMU/caches; native entry must never return",
                        "Existing Tegra bootm cleanup writes USB/UPHY controls and sends GICv2 SGI0; it is not a read-only register probe",
                        "If a kernel returns, existing U-Boot resets before the script can regain control",
                        "Script refusal paths hold; firmware faults or manual interruption are outside that guarantee",
                        "No persistent storage-write, saveenv, reset or alternative boot-target command is authored"],
    }
    output.mkdir(parents=True, exist_ok=False)
    for path, data in artifacts.items():
        destination = output / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "build-readelf.txt").write_text(elf_report)
    (output / "build-disassembly.txt").write_text(disassembly)
    (output / "handoff.elf").write_bytes(elf)
    for path, expected in manifest["files"].items():
        if record((output / path).read_bytes()) != expected:
            raise ValueError(f"staged readback mismatch: {path}")
    (output / "verification.json").write_text(json.dumps({
        "all_staged_files_rehashed": True, "legacy_header_and_data_crcs_verified": True,
        "firmware_and_original_oled_dtb_pins_verified": True,
        "elf_entry_and_load_segments_bounded": True, "elf_has_no_relocations": True,
        "independent_review_pending": True, "hardware_execution_verified": False}, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/native-handoff-v2-fat32")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    try:
        result = build(args.output, args.binary, args.elf)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        stderr = getattr(exc, "stderr", None)
        parser.exit(1, f"native handoff packaging failed: {exc}\n{stderr or ''}")
    print(json.dumps({"output": str(args.output.absolute()), "status": result["status"],
                      "files": result["files"]}, indent=2))


if __name__ == "__main__":
    main()
