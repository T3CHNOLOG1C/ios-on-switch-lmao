#!/usr/bin/env python3
"""Package exact disk bytes into FAT32-sized parts; this is not a boot loader."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

FAT32_MAX = 2**32 - 1
DEFAULT_PART_BYTES = 4_000_000_000
BLOCK_BYTES = 4096
IO_BYTES = 4 * 1024 * 1024


def regular(path):
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError(f"Expected a regular file, not a device or symlink: {path}")


def part_size(value):
    number = int(value)
    if not 512 <= number <= FAT32_MAX or number % 512:
        raise ValueError("Part size must be a multiple of 512 below 4 GiB")
    return number


def pack(source, output, chunk_bytes=DEFAULT_PART_BYTES):
    source, output = Path(source), Path(output)
    regular(source)
    chunk_bytes = part_size(chunk_bytes)
    size = source.stat().st_size
    if size <= 0 or size % 512:
        raise ValueError("Expected a nonempty sector-aligned raw disk")
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": "ioswitch-split-volume-v1", "native_bootable": False,
                "note": "Storage packaging only. A native SD/FAT32 block reader and Switch kernel are not supplied.",
                "size_bytes": size, "part_size_bytes": chunk_bytes, "parts": []}
    digest = hashlib.sha256()
    consumed = 0
    with source.open("rb") as stream:
        for index in range((size + chunk_bytes - 1) // chunk_bytes):
            name = f"part-{index:04d}.bin"
            expected = min(chunk_bytes, size - consumed)
            current = hashlib.sha256()
            count = 0
            with (output / name).open("xb") as target:
                while count < expected:
                    data = stream.read(min(IO_BYTES, expected - count))
                    if not data:
                        raise ValueError("Source was truncated while packaging")
                    target.write(data)
                    digest.update(data)
                    current.update(data)
                    count += len(data)
            manifest["parts"].append({"file": name, "offset": consumed,
                                      "size_bytes": count, "sha256": current.hexdigest()})
            consumed += count
        if stream.read(1):
            raise ValueError("Source grew while packaging")
    manifest["sha256"] = digest.hexdigest()
    # Publish the descriptor last. A failed pack cannot look complete.
    with (output / "volume.json").open("x") as descriptor:
        json.dump(manifest, descriptor, indent=2)
        descriptor.write("\n")
    return manifest


class SplitVolume:
    """Read-only byte mapping for host verification; not a native SD driver."""
    def __init__(self, directory):
        self.directory = Path(directory)
        descriptor = self.directory / "volume.json"
        regular(descriptor)
        self.manifest = manifest = json.loads(descriptor.read_text())
        if not isinstance(manifest, dict) or manifest.get("schema") != "ioswitch-split-volume-v1":
            raise ValueError("Unsupported split-volume descriptor")
        self.size = manifest.get("size_bytes")
        if type(self.size) is not int or self.size <= 0 or self.size % 512:
            raise ValueError("Invalid total disk size")
        self.part_bytes = manifest.get("part_size_bytes")
        if type(self.part_bytes) is not int:
            raise ValueError("Invalid part size")
        part_size(self.part_bytes)
        parts = manifest.get("parts")
        if not isinstance(parts, list) or len(parts) != (self.size + self.part_bytes - 1) // self.part_bytes:
            raise ValueError("Wrong part count")
        self.paths = []
        for index, part in enumerate(parts):
            if not isinstance(part, dict):
                raise ValueError("Malformed part")
            offset = index * self.part_bytes
            size = min(self.part_bytes, self.size - offset)
            name = f"part-{index:04d}.bin"
            if part.get("file") != name or part.get("offset") != offset or part.get("size_bytes") != size:
                raise ValueError("Part names, offsets and sizes must describe one contiguous disk")
            if not re.fullmatch(r"[0-9a-f]{64}", str(part.get("sha256", ""))):
                raise ValueError("Invalid part digest")
            path = self.directory / name
            regular(path)
            if path.stat().st_size != size:
                raise ValueError(f"Part is truncated or oversized: {name}")
            self.paths.append(path)
        if not re.fullmatch(r"[0-9a-f]{64}", str(manifest.get("sha256", ""))):
            raise ValueError("Invalid volume digest")

    def read_at(self, offset, length):
        if type(offset) is not int or type(length) is not int or offset < 0 or length < 0 or length > IO_BYTES:
            raise ValueError("Read must be nonnegative and at most 4 MiB")
        if offset > self.size or length > self.size - offset:
            raise ValueError("Read exceeds disk bounds")
        chunks = []
        while length:
            index, within = divmod(offset, self.part_bytes)
            count = min(length, self.part_bytes - within)
            with self.paths[index].open("rb") as stream:
                stream.seek(within)
                data = stream.read(count)
            if len(data) != count:
                raise ValueError("Part changed or was truncated during read")
            chunks.append(data)
            offset += count
            length -= count
        return b"".join(chunks)

    def verify(self):
        whole = hashlib.sha256()
        for path, record in zip(self.paths, self.manifest["parts"]):
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for data in iter(lambda: stream.read(IO_BYTES), b""):
                    whole.update(data)
                    digest.update(data)
            if digest.hexdigest() != record["sha256"]:
                raise ValueError(f"Digest mismatch: {path.name}")
        if whole.hexdigest() != self.manifest["sha256"]:
            raise ValueError("Whole-disk digest mismatch")
        return {"verified": True, "size_bytes": self.size, "sha256": whole.hexdigest(),
                "parts": len(self.paths), "all_files_fat32_sized": True,
                "native_bootable": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("pack")
    create.add_argument("--source", required=True, type=Path)
    create.add_argument("--output", required=True, type=Path)
    create.add_argument("--part-bytes", type=int, default=DEFAULT_PART_BYTES)
    check = commands.add_parser("verify")
    check.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "pack":
            result = pack(args.source, args.output, args.part_bytes)
        else:
            result = SplitVolume(args.directory).verify()
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"split volume: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
