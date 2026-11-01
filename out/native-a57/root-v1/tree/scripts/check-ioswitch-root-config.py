#!/usr/bin/env python3
"""Fail closed before mutating the isolated XNU tree for verified SD root.

This checks composition, not runtime authentication. Each archive builder and
the final candidate audit separately verify actual objects and original ABI.
"""
from pathlib import Path
import hashlib
import json
import os
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_crypto_archive(root, archive):
    report = json.loads(archive.with_name("build.json").read_text())
    if (Path(report["archive"]).resolve() != archive or
            report["archive_sha256"] != digest(archive) or
            report["archive_bytes"] != archive.stat().st_size or
            not 0 < archive.stat().st_size <= 256 * 1024 or
            report["public_key_bytes"] != 395 or
            report["trusted_key_sha256"] != "b461cf5e80ed2cc20a61abc5c18b280567c4383cd92edf83867407e560ddc53b" or
            any(report.get(name) is not True for name in
                ("embedded_tables_absent", "legacy_probe_absent", "original_crypto_adapter"))):
        raise ValueError("Rebuild the exact key-only IOSwitch crypto archive")
    sources = report["source_sha256"]
    objects = report["object_sha256"]
    if set(sources) != {"kits/VerifiedRoot/src/verified_root.c", "kits/VerifiedRoot/adapters/xnu_crypto.c",
                        "build/verified-root-ioswitch/trusted-key.S"} or set(objects) != {
                            "verified_root.o", "xnu_crypto.o", "trusted-key.o"}:
        raise ValueError("Unexpected key-only verifier source or object set")
    for name, expected in sources.items():
        if digest(root / name) != expected:
            raise ValueError("Rebuild the IOSwitch verifier after changing " + name)
    for name, expected in objects.items():
        if digest(archive.parent / name) != expected:
            raise ValueError("Rebuild the IOSwitch verifier object " + name)


def validate_block_archive(root, archive):
    report = json.loads(archive.with_name("bindings.json").read_text())
    files = {
        "archive_sha256": archive,
        "compiled_object_sha256": archive.with_name("NativeIOSwitchBlockDevice.o"),
        "binding_object_sha256": archive.with_name("original-bindings.o"),
        "source_sha256": root / "drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp",
        "interface_sha256": root / "diagnostics/ioswitch/storage/block_backend.h",
        "root_policy_header_sha256": root / "diagnostics/ioswitch/IOSwitchRootPolicy.h",
        "verified_root_header_sha256": root / "kits/VerifiedRoot/include/verified_root.h",
        "storage_audit_sha256": root / "build/storage-abi/interface-audit.json",
        "original_candidate_sha256": root / "build/native-prelink/native-apfs-load-positive.macho",
    }
    if (report["virtual_methods_checked"] != 190 or report["unresolved_original_imports"] != 0 or
            report.get("original_code_modified") is not False):
        raise ValueError("Require the reviewed original storage ABI for the native root driver")
    for name, path in files.items():
        if report[name] != digest(path):
            raise ValueError("Rebuild the IOSwitch block archive after changing " + str(path))


def validate(environment, root, *, require_archives=True):
    mode = environment.get("XNU_IOSWITCH_VERIFIED_ROOT", "0")
    if mode not in ("0", "1"):
        raise ValueError("XNU_IOSWITCH_VERIFIED_ROOT must be 0 or 1")
    if mode == "0":
        return {}
    required = {
        "XNU_BOOTSTRAP": "0",
        "XNU_IOSWITCH_DIAGNOSTICS": "1",
        "XNU_SOURCE_CORECRYPTO14": "1",
        "XNU_CRYPTO_BOOTSTRAP": "prelinked",
        "XNU_NATIVE_AES": "1",
        "XNU_TRUST_CACHE_RUNTIME": "1",
        "XNU_NATIVE_CS_HASH_CACHE": "1",
        "XNU_VERIFIED_ROOT_PROBE": "1",
        "XNU_OWNER_ROOT_AUDIT": "1",
        "XNU_NATIVE_VIRTIO_BLOCK": "0",
        "XNU_NATIVE_UFS": "0",
    }
    for name, expected in required.items():
        default = "0" if name != "XNU_CRYPTO_BOOTSTRAP" else "source"
        if environment.get(name, default) != expected:
            raise ValueError(f"Verified IOSwitch root requires {name}={expected}")
    # OWNER_ROOT_TRUST=0 is deliberately supported for the authentication-refusal
    # control. It never disables per-read block verification in this provider.
    if environment.get("XNU_OWNER_ROOT_TRUST", "0") not in ("0", "1"):
        raise ValueError("XNU_OWNER_ROOT_TRUST must be 0 or 1")
    root = Path(root).resolve()
    archives = {
        "XNU_IOSWITCH_BLOCK_ARCHIVE": root / "build/native-ioswitch-root/libNativeIOSwitchBlockDevice.a",
        "XNU_VERIFIED_ROOT_ARCHIVE": root / "build/verified-root-ioswitch/libVerifiedRootProbe.a",
    }
    for name, expected in archives.items():
        selected = Path(environment.get(name, str(expected)))
        if selected.resolve() != expected:
            raise ValueError(f"{name} must select this isolated tree's native root archive: {expected}")
        if require_archives and (selected.is_symlink() or not selected.is_file() or not selected.stat().st_size):
            raise ValueError(f"Build the isolated native root archive first: {expected}")
    if require_archives:
        validate_crypto_archive(root, archives["XNU_VERIFIED_ROOT_ARCHIVE"])
        validate_block_archive(root, archives["XNU_IOSWITCH_BLOCK_ARCHIVE"])
    return archives


if __name__ == "__main__":
    try:
        validate(os.environ, Path(__file__).resolve().parents[1])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
