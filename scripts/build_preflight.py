#!/usr/bin/env python3
"""Read-only Mac build inventory; never downloads, installs, mounts or builds.

The 50 GiB reserve is a planning estimate, not a measured full-build maximum.
Passing these checks does not prove the source pairing compiles or iPadOS boots.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

GIB = 1 << 30
DEFAULT_FREE_GIB = 50
CANDIDATE_UNIXNU = "1dba43325b8242c3c55001999cda841c25e9ca68"
QEMU_DEVICE = "97b75b1f63c6b7e895cd7646029db741d5fe5488"
EXTRA_BIN = (
    "/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin",
    "/usr/local/opt/llvm/bin", "/usr/local/opt/lld/bin",
)
TOOLS = (
    "git", "make", "cmake", "ninja", "unifdef", "mcopy", "mformat", "dtc",
    "qemu-system-aarch64", "xcrun", "xcodebuild", "hdiutil", "diskutil",
    "clang", "lld-link", "ld.lld", "llvm-lib", "llvm-rc", "llvm-objcopy", "llvm-nm",
)
FONT_PATH = Path("Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h")


def find_tool(name: str) -> str | None:
    search = os.pathsep.join([os.environ.get("PATH", ""), *EXTRA_BIN])
    return shutil.which(name, path=search)


def query(command: list[str]) -> tuple[bool, str]:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=20, check=False)
        return result.returncode == 0, (result.stdout or result.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return False, str(error)


def storage_check(path: Path, minimum_gib: int) -> dict:
    if minimum_gib <= 0:
        raise ValueError("minimum free space must be positive")
    if not path.is_dir():
        raise ValueError("scratch path must be an existing directory; nothing was created")
    resolved = path.resolve()
    free = shutil.disk_usage(resolved).free
    return {"path": str(resolved), "free_bytes": free, "minimum_bytes": minimum_gib * GIB,
            "ok": free >= minimum_gib * GIB, "budget_is_estimate": True}


def inspect(args: argparse.Namespace) -> dict:
    storage = storage_check(args.scratch, args.minimum_free_gib)
    checks = []

    def add(name: str, ok: bool, detail: str):
        checks.append({"name": name, "ok": ok, "detail": detail})

    add("macOS", platform.system() == "Darwin", platform.platform())
    add("Python 3.11+", sys.version_info >= (3, 11), sys.version.split()[0])
    add("working space", storage["ok"], f"{storage['free_bytes'] / GIB:.2f} GiB free; "
        f"{args.minimum_free_gib} GiB planning reserve")
    tools = {name: find_tool(name) for name in TOOLS}
    for name, executable in tools.items():
        add(name, executable is not None, executable or "not found")
    if tools["xcrun"]:
        ok, sdk = query([tools["xcrun"], "--sdk", "iphoneos", "--show-sdk-path"])
        add("iPhoneOS SDK", ok and Path(sdk).is_dir(), sdk)
    for name, flags in (("xcodebuild", ["-version"]), ("qemu-system-aarch64", ["--version"])):
        if tools[name]:
            ok, version = query([tools[name], *flags])
            add(name + " version", ok, version)
    if tools["qemu-system-aarch64"]:
        code = Path(tools["qemu-system-aarch64"]).resolve().parents[1] / "share/qemu/edk2-aarch64-code.fd"
        add("AArch64 UEFI", code.is_file(), str(code))
    prefix = args.openssl_prefix
    for relative in ("include/openssl/evp.h", "lib/libcrypto.dylib"):
        file = prefix / relative
        add("OpenSSL 3 " + relative, file.is_file(), str(file))
    font = args.uefi / FONT_PATH if args.uefi else None
    add("panic screen font", font is not None and font.is_file(),
        str(font) if font else "set --uefi to the pinned UEFI source containing Font.h")
    for label, path, expected in (("UniXNU", args.unixnu, CANDIDATE_UNIXNU),
                                   ("QEMU device", args.device, QEMU_DEVICE)):
        if path is None:
            add(label + " checkout", False, "supply its source path")
            continue
        if not path.is_dir() or not tools["git"]:
            add(label + " checkout", False, "source directory or git missing")
            continue
        ok, revision = query([tools["git"], "-C", str(path), "rev-parse", "HEAD"])
        add(label + " candidate revision", ok and revision == expected,
            f"observed {revision}; candidate {expected}; pair not yet build-verified")
        if label == "UniXNU":
            ok, status = query([tools["git"], "-C", str(path), "submodule", "status", "--recursive"])
            problems = [line for line in status.splitlines() if line[:1] in ("-", "+", "U")]
            add("pinned submodules", ok and not problems, "\n".join(problems) or status or "none listed")
    return {"schema_version": 1, "host_checks_passed": all(check["ok"] for check in checks),
            "build_or_boot_verified": False, "storage": storage, "checks": checks,
            "note": "Read-only inventory. Candidate pins and 50 GiB budget still need a real build."}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch", type=Path, default=Path.cwd())
    parser.add_argument("--minimum-free-gib", type=int, default=DEFAULT_FREE_GIB)
    parser.add_argument("--unixnu", type=Path)
    parser.add_argument("--device", type=Path)
    parser.add_argument("--uefi", type=Path)
    parser.add_argument("--openssl-prefix", type=Path, default=Path("/opt/homebrew/opt/openssl@3"))
    args = parser.parse_args(argv)
    try:
        report = inspect(args)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0 if report["host_checks_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
