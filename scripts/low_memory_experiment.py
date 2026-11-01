#!/usr/bin/env python3
"""Isolated Mac-only 4 GiB/2.5 GiB payload experiment; no production RAM override."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTS_ROOT = ROOT / "out/experiments"
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import ioswitch
from scripts import export_unixnu as exporter

PROFILE = "unixnu-ipados-4g-2p5g-experimental"
ADT = "esp/EFI/UniXNU/Payloads/kernel.macho.adt"
CONTRACT = {"schema_version": 1, "profile": PROFILE, "ram_mib": 4096,
            "owned_mib": 2560, "maxvnodes": 41984, "tcg_cache_mib": 256,
            "seconds": 900, "screen": "1376x1024", "cpu": "cortex-a710", "smp": 1}
CHANGES = [("chosen", "dram-size", 8, 6144 << 20, 4096 << 20),
           ("chosen", "darwinboot-memory-size", 8, 4096 << 20, 2560 << 20),
           ("defaults", "kern.maxvnodes", 4, 66560, 41984)]


class ExperimentError(ValueError):
    pass


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def safe_output(path):
    path = path.absolute()
    if any(c in str(path) for c in (",", "\n")):
        raise ExperimentError("experiment path must not contain commas or newlines")
    allowed = EXPERIMENTS_ROOT.resolve()
    if path.resolve() == allowed or not path.resolve().is_relative_to(allowed):
        raise ExperimentError("experiment output must be inside the dedicated out/experiments directory")
    # Refuse aliases even when they happen to resolve within the allowed root.
    for candidate in (path, *path.parents):
        if candidate.is_symlink():
            raise ExperimentError("experiment path must not contain symlinks")
    return path.resolve()


def serialize_adt(node):
    props, children = node["properties"], node["children"]
    blob = struct.pack("<II", len(props), len(children))
    for key, value in props.items():
        blob += struct.pack("<32sI", key.encode("ascii"), len(value))
        blob += value + bytes(-len(value) % 4)
    return blob + b"".join(map(serialize_adt, children))


def changed_adt(reference):
    exporter.validate_adt(reference)
    tree = exporter.parse_adt(reference)
    if serialize_adt(tree) != reference:
        raise ExperimentError("reference ADT does not round-trip byte-for-byte")
    for node_name, key, width, old, new in CHANGES:
        node = exporter.child(tree, node_name)
        if exporter.number_property(node, key, width) != old:
            raise ExperimentError(f"unexpected reference /{node_name}/{key}")
        node["properties"][key] = new.to_bytes(width, "little")
    return serialize_adt(tree)


def validate_derivative(reference, derivative):
    # Full byte equality to the reconstructed derivative rejects *any* fourth
    # property, padding, ordering, identity, trust or CPU/timer change.
    if derivative != changed_adt(reference):
        raise ExperimentError("experimental ADT differs from the exact three-property edit")
    return exporter.parse_adt(derivative)


def parse_fdt(blob):
    """Read the bounded FDT structure QEMU emits; return absolute node paths."""
    if len(blob) < 40 or len(blob) > 4 << 20:
        raise ExperimentError("invalid FDT size")
    magic, total, off, strings_off, _, version, compat, _, strings_size, size = struct.unpack_from(">10I", blob)
    if (magic != 0xD00DFEED or total > len(blob) or total < 40 or version != 17 or compat > 17
            or off < 40 or off + size > total or strings_off < 40 or strings_off + strings_size > total):
        raise ExperimentError("invalid FDT header")
    data, strings = blob[off:off + size], blob[strings_off:strings_off + strings_size]
    at, stack, nodes = 0, [], {}

    def read(size):
        nonlocal at
        if size < 0 or at + size > len(data):
            raise ExperimentError("truncated FDT structure")
        result = data[at:at + size]
        at += size
        return result

    while at < len(data):
        token, = struct.unpack(">I", read(4))
        if token == 1:
            end = data.find(b"\0", at)
            if end < at:
                raise ExperimentError("unterminated FDT node")
            name = read(end - at + 1)[:-1].decode("ascii")
            read(-at % 4)
            if "/" in name or len(stack) > 64 or (not stack and (name or nodes)):
                raise ExperimentError("invalid FDT nesting")
            stack.append(name)
            path = "/" + "/".join(stack[1:])
            if path in nodes:
                raise ExperimentError("duplicate FDT node")
            nodes[path] = {}
        elif token == 2:
            if not stack:
                raise ExperimentError("unbalanced FDT node")
            stack.pop()
        elif token == 3:
            if not stack:
                raise ExperimentError("FDT property outside node")
            length, index = struct.unpack(">II", read(8))
            end = strings.find(b"\0", index)
            if index >= len(strings) or end < index:
                raise ExperimentError("invalid FDT property name")
            name = strings[index:end].decode("ascii")
            props = nodes["/" + "/".join(stack[1:])]
            if not name or name in props:
                raise ExperimentError("duplicate or empty FDT property")
            props[name] = read(length)
            read(-at % 4)
        elif token == 4:
            continue
        elif token == 9:
            if stack or "/" not in nodes:
                raise ExperimentError("incomplete FDT")
            return nodes
        else:
            raise ExperimentError(f"invalid FDT token {token}")
    raise ExperimentError("missing FDT terminator")


def validate_fdt(blob, adt):
    nodes = parse_fdt(blob)
    root = nodes["/"]
    if any(root.get(key) != struct.pack(">I", 2) for key in ("#address-cells", "#size-cells")):
        raise ExperimentError("FDT root must use two address and size cells")
    memory = [p for p in nodes.values() if p.get("device_type") == b"memory\0"]
    if len(memory) != 1 or memory[0].get("reg") != struct.pack(">QQ", 0x40000000, 4096 << 20):
        raise ExperimentError("QEMU FDT memory does not match 4096 MiB profile")
    cpus = [p for p in nodes.values() if p.get("device_type") == b"cpu\0"]
    if len(cpus) != 1 or b"arm,cortex-a710" not in cpus[0].get("compatible", b"").split(b"\0"):
        raise ExperimentError("QEMU FDT must describe one Cortex-A710")
    armio = exporter.child(adt, "arm-io")
    ranges = armio["properties"].get("ranges", b"")
    if len(ranges) != 24:
        raise ExperimentError("unsupported ADT arm-io mapping")
    child_base, parent_base, span = struct.unpack("<QQQ", ranges)

    def windows(value, endian, translate=False):
        if not value or len(value) % 16:
            raise ExperimentError("invalid MMIO register windows")
        result = []
        for offset in range(0, len(value), 16):
            base, size = struct.unpack_from(endian + "QQ", value, offset)
            if translate:
                if base < child_base or base + size > child_base + span:
                    raise ExperimentError("ADT MMIO exceeds arm-io range")
                base += parent_base - child_base
            result.append((base, size))
        return result

    def compatible(props, name):
        return name in props.get("compatible", b"").split(b"\0")

    gic = [p for p in nodes.values() if compatible(p, b"arm,gic-v3")]
    intc = exporter.child(armio, "intc")["properties"]
    if (len(gic) != 1 or not compatible(intc, b"arm,gic-v3")
            or windows(gic[0].get("reg", b""), ">") != windows(intc.get("reg", b""), "<", True)):
        raise ExperimentError("QEMU GICv3 windows do not match ADT")
    actual, expected = [], []
    for props in nodes.values():
        if compatible(props, b"virtio,mmio"):
            if "dma-coherent" not in props:
                raise ExperimentError("QEMU VirtIO lacks DMA coherency")
            actual.extend(windows(props.get("reg", b""), ">"))
    for node in armio["children"]:
        props = node["properties"]
        if compatible(props, b"virtio,mmio"):
            if "dma-coherent" not in props:
                raise ExperimentError("ADT VirtIO lacks DMA coherency")
            expected.extend(windows(props.get("reg", b""), "<", True))
    if not actual or sorted(actual) != sorted(expected):
        raise ExperimentError("QEMU VirtIO MMIO windows do not match ADT")
    return {"dram_base": 0x40000000, "dram_bytes": 4096 << 20,
            "cpu_count": 1, "gic": "v3", "virtio_mmio_windows": len(actual)}


def tree_records(esp):
    records = {}
    for path in sorted(esp.rglob("*")):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ExperimentError(f"unsupported staged ESP entry: {path}")
        if path.is_file():
            records["esp/" + path.relative_to(esp).as_posix()] = exporter.file_record(path)
    return records


def experiment_command(root, manifest, output, run):
    if any(c in str(output) for c in (",", "\n")):
        raise ExperimentError("experiment path must not contain commas or newlines")
    command = ioswitch.ipad_command(root, {**manifest, "ram_mib": 4096}, run,
                                   "none", tcg_cache_mib=256)
    old = f"if=none,id=esp,format=raw,readonly=on,file=fat:ro:{root / manifest['esp']}"
    if command.count(old) != 1:
        raise ExperimentError("reference ESP drive topology changed")
    command[command.index(old)] = f"if=none,id=esp,format=raw,readonly=on,file=fat:ro:{output / 'esp'}"
    return command


def dump_fdt(output):
    output = safe_output(output)
    qemu = shutil.which("qemu-system-aarch64")
    if not qemu:
        raise ExperimentError("qemu-system-aarch64 missing")
    command = [qemu, "-machine", f"virt,virtualization=off,secure=off,gic-version=3,dumpdtb={output / 'qemu-4g.dtb'}",
               "-accel", "tcg,tb-size=256", "-cpu", "cortex-a710", "-m", "4096", "-smp", "1",
               "-display", "none", "-nodefaults"]
    write_json(output / "fdt-command.json", command)
    version = subprocess.check_output([qemu, "--version"], text=True, timeout=10)
    completed = subprocess.run(command, capture_output=True, text=True, timeout=20)
    (output / "fdt.log").write_text(completed.stdout + completed.stderr)
    if completed.returncode:
        raise ExperimentError("QEMU FDT generation failed; inspect fdt.log")
    return version


def prepare(reference, output):
    reference, output = reference.resolve(), output.absolute()
    if output.exists() or output.is_symlink():
        raise ExperimentError("output already exists; choose a new experiment directory")
    if output.resolve().is_relative_to(reference):
        raise ExperimentError("output must be outside the reference bundle")
    output = safe_output(output)
    root, manifest = ioswitch.load_bundle(reference)
    if manifest["screen"] != CONTRACT["screen"]:
        raise ExperimentError("reference screen differs from this experiment")
    exporter.validate_esp(root / manifest["esp"])
    original = (root / ADT).read_bytes()
    derivative = changed_adt(original)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Never overwrite or clean up a caller-owned directory.
    shutil.copytree(root / manifest["esp"], output / "esp")
    (output / ADT).write_bytes(derivative)
    expected = {name: record for name, record in manifest["files"].items() if name.startswith("esp/")}
    expected[ADT] = {"sha256": hashlib.sha256(derivative).hexdigest(), "size_bytes": len(derivative)}
    if tree_records(output / "esp") != expected:
        raise ExperimentError("copied ESP does not match reference plus exact ADT edit")
    qemu_version = dump_fdt(output)
    fdt_validation = validate_fdt((output / "qemu-4g.dtb").read_bytes(), exporter.parse_adt(derivative))
    shutil.copyfile(root / "manifest.json", output / "reference-manifest.json")
    receipt = {**CONTRACT, "reference": str(root),
               "reference_manifest_sha256": ioswitch.sha256(root / "manifest.json"),
               "reference_adt_sha256": hashlib.sha256(original).hexdigest(),
               "adt_changes": [{"path": f"/{node}/{key}", "old": old, "new": new, "width": width}
                               for node, key, width, old, new in CHANGES],
               "files": expected, "reference_files": manifest["files"],
               "source_pins": {"unixnu": "1dba43325b8242c3c55001999cda841c25e9ca68",
                               "unixnu_qemu": manifest["upstream"]["revision"],
                               "darwinboot": "85332b317ee80898fc78939ea64e6ca52d3837a3"},
               "fdt": exporter.file_record(output / "qemu-4g.dtb"),
               "fdt_validation": fdt_validation, "qemu_version": qemu_version,
               "boot_verified": False, "switch_verified": False}
    write_json(output / "experiment.json", receipt)
    return receipt


def validate_experiment(output):
    output = safe_output(output)
    receipt = json.loads((output / "experiment.json").read_text())
    for key, expected in CONTRACT.items():
        if receipt.get(key) != expected:
            raise ExperimentError(f"unsupported experimental {key}")
    root, manifest = ioswitch.load_bundle(receipt["reference"])
    if output.is_relative_to(root):
        raise ExperimentError("experiment must be outside reference")
    if (ioswitch.sha256(root / "manifest.json") != receipt["reference_manifest_sha256"]
            or ioswitch.sha256(output / "reference-manifest.json") != receipt["reference_manifest_sha256"]
            or manifest["files"] != receipt["reference_files"]):
        raise ExperimentError("reference manifest changed")
    exporter.validate_esp(root / manifest["esp"])
    derivative = (output / ADT).read_bytes()
    tree = validate_derivative((root / ADT).read_bytes(), derivative)
    expected = {name: record for name, record in manifest["files"].items() if name.startswith("esp/")}
    expected[ADT] = {"sha256": hashlib.sha256(derivative).hexdigest(), "size_bytes": len(derivative)}
    if receipt["files"] != expected or tree_records(output / "esp") != expected:
        raise ExperimentError("staged ESP changed")
    if exporter.file_record(output / "qemu-4g.dtb") != receipt["fdt"]:
        raise ExperimentError("QEMU FDT changed")
    validate_fdt((output / "qemu-4g.dtb").read_bytes(), tree)
    version = subprocess.check_output(["qemu-system-aarch64", "--version"], text=True, timeout=10)
    if version != receipt["qemu_version"]:
        raise ExperimentError("QEMU version changed since the FDT was generated")
    if manifest["screen"] != CONTRACT["screen"]:
        raise ExperimentError("reference screen changed")
    return root, manifest, receipt


def run_experiment(output):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ExperimentError("this first experiment is restricted to the development arm64 Mac")
    output = safe_output(output)
    runs = output / "runs"
    if runs.is_symlink() or (runs.exists() and not runs.is_dir()) or runs.resolve().parent != output:
        raise ExperimentError("runs must be a real directory directly inside the experiment")
    root, manifest, receipt = validate_experiment(output)
    run = ioswitch.new_run(runs)
    ioswitch.make_vars(run)
    write_json(run / "experiment.json", receipt)
    try:
        result = ioswitch.execute(experiment_command(root, manifest, output, run), run, 900, capture_every=20)
    finally:
        # execute also writes a result before raising on an emulator failure.
        # Preserve its diagnostics while ensuring failures remain experimental.
        if (run / "result.json").is_file():
            result = json.loads((run / "result.json").read_text())
            result.update(profile=PROFILE, experimental_contract=CONTRACT, ipad_ui_verified=False)
            write_json(run / "result.json", result)
        integrity = {"passed": False}
        try:
            _, after, _ = validate_experiment(output)
            if after != manifest:
                raise ExperimentError("reference manifest content changed during run")
            integrity = {"passed": True, "reference_manifest_sha256": receipt["reference_manifest_sha256"],
                         "all_reference_and_staged_files_rehashed": True}
        except Exception as exc:
            integrity["error"] = str(exc)
            raise
        finally:
            write_json(run / "post-run-integrity.json", integrity)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("--reference", type=Path, required=True)
    prep.add_argument("--output", type=Path, required=True)
    run = sub.add_parser("run")
    run.add_argument("--experiment", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.reference, args.output) if args.command == "prepare" else run_experiment(args.experiment)
        print(json.dumps(result, indent=2))
    except (ExperimentError, exporter.ExportError, ioswitch.Error, OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"low-memory experiment: {exc}\n")


if __name__ == "__main__":
    main()
