#!/usr/bin/env python3
"""Export the exact read-only OS image authenticated by UniXNU's root policy.

Accepts an image path, never a disk identifier. Uses only the virtual disk
returned for that image by hdiutil, verifies its identity, copies to a new file,
and requires the resulting hash to match the paired policy report.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import tempfile

OS_BYTES = 6_077_580_800
SOURCE_SHA256 = "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d"
OS_UUID = "04769D26-2CA0-4825-B443-E2E5F88300F2"
CHUNK = 8 << 20
RESERVE = 10 << 30


class ExportError(ValueError):
    pass


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            value.update(block)
    return value.hexdigest()


def plist(*command: str) -> dict:
    return plistlib.loads(subprocess.check_output(command))


def validate_policy(policy: dict) -> None:
    if (not isinstance(policy, dict)
            or policy.get("image_bytes") != OS_BYTES or policy.get("volume_uuid") != OS_UUID
            or policy.get("source_compressed_sha256") != SOURCE_SHA256
            or policy.get("real_c_signature_and_block_verification") is not True
            or policy.get("original_image_modified") is not False
            or not isinstance(policy.get("image_sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", policy.get("image_sha256", ""))):
        raise ExportError("require the paired, fully verified iPad7,1 OS policy report")


def whole_disk(attachment: dict) -> str:
    whole = [entry.get("dev-entry") for entry in attachment.get("system-entities", [])
             if entry.get("content-hint") == "GUID_partition_scheme"]
    if len(whole) != 1 or not isinstance(whole[0], str) or not re.fullmatch(r"/dev/disk[0-9]+", whole[0]):
        raise ExportError("attachment must contain exactly one whole virtual GUID disk")
    return whole[0]


def matching_images(images: dict, source: Path) -> list[dict]:
    return [item for item in images.get("images", [])
            if item.get("image-path") and Path(item["image-path"]).resolve() == source]


def validate_mapping(images: dict, source: Path, device: str, info: dict) -> str:
    matches = matching_images(images, source)
    if (len(matches) != 1 or matches[0].get("writeable") is not False
            or whole_disk(matches[0]) != device):
        raise ExportError("attached mapping does not uniquely identify the exact read-only source image")
    if (info.get("BusProtocol") != "Disk Image" or info.get("Writable") is not False
            or info.get("DeviceNode") != device or info.get("TotalSize") != OS_BYTES):
        raise ExportError("device is not the expected read-only virtual media")
    return "/dev/r" + Path(device).name


def detach_owned_image(source: Path, device: str) -> None:
    # Device numbers can be reused after an external detach. Recheck ownership
    # before ejecting anything, including after a failed copy or validation.
    matches = matching_images(plist("hdiutil", "info", "-plist"), source)
    if not matches:
        return  # The image was already detached; never eject a reused number.
    if len(matches) != 1 or whole_disk(matches[0]) != device:
        raise ExportError(f"cannot safely detach changed image mapping for {source}")
    info = plist("diskutil", "info", "-plist", device)
    if info.get("BusProtocol") != "Disk Image" or info.get("DeviceNode") != device:
        raise ExportError(f"cannot safely detach unverified virtual device {device}")
    subprocess.run(["hdiutil", "detach", device], check=True, capture_output=True, text=True)


@contextmanager
def attached_image(image: Path):
    attachment = plist("hdiutil", "attach", "-nomount", "-readonly", "-noverify", "-noautofsck", "-plist", str(image))
    try:
        device = whole_disk(attachment)
    except ExportError as error:
        raise ExportError(f"{error}; attachment for {image} could not be identified for safe cleanup") from error
    try:
        yield device
    except BaseException as error:
        try:
            detach_owned_image(image, device)
        except Exception as cleanup_error:
            raise ExportError(f"{error}; image cleanup also failed: {cleanup_error}") from error
        raise
    else:
        detach_owned_image(image, device)


def copy_verified(source, target, expected_size: int, expected_sha256: str) -> str:
    complete = hashlib.sha256()
    remaining = expected_size
    while remaining:
        block = source.read(min(CHUNK, remaining))
        if not block:
            raise ExportError("virtual media ended before the authenticated boundary")
        target.write(block)
        complete.update(block)
        remaining -= len(block)
    if source.read(1) or complete.hexdigest() != expected_sha256:
        raise ExportError("raw OS bytes do not match the paired authenticated policy")
    return complete.hexdigest()


def export(image: Path, verification: Path, output: Path) -> dict:
    image = image.resolve(strict=True)
    policy_bytes = verification.read_bytes()
    policy = json.loads(policy_bytes)
    validate_policy(policy)
    if not image.is_file() or digest(image) != SOURCE_SHA256:
        raise ExportError("compressed OS member does not match its pinned source digest")
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ExportError("output must be a new file in an existing directory")
    if shutil.disk_usage(output.parent).free < OS_BYTES + RESERVE:
        raise ExportError("export must retain at least 10 GiB free")
    if matching_images(plist("hdiutil", "info", "-plist"), image):
        raise ExportError("source image is already attached; finish its current run before exporting")
    temporary = None
    try:
        with attached_image(image) as device:
            raw = validate_mapping(plist("hdiutil", "info", "-plist"), image, device,
                                   plist("diskutil", "info", "-plist", device))
            descriptor = os.open(raw, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(descriptor, "rb", buffering=CHUNK) as source:
                if not stat.S_ISCHR(os.fstat(source.fileno()).st_mode):
                    raise ExportError("validated raw mapping is not a character device")
                with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".ioswitch-os-", suffix=".partial", delete=False) as target:
                    temporary = Path(target.name)
                    raw_sha = copy_verified(source, target, OS_BYTES, policy["image_sha256"])
                    target.flush()
                    os.fsync(target.fileno())
        # Publish only after the owned image has been successfully detached.
        # A hard link within this directory is atomic and refuses an existing destination.
        os.link(temporary, output)
        return {"output": str(output.resolve()), "image_bytes": OS_BYTES, "image_sha256": raw_sha,
                "source": str(image), "source_sha256": SOURCE_SHA256,
                "policy_sha256": hashlib.sha256(policy_bytes).hexdigest(), "paired_policy_matched": True,
                "kernel_policy_linkage_verified": False, "guest_boot_verified": False}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--verification", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = export(args.image, args.verification, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"error: {error}\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
