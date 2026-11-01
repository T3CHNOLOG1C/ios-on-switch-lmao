#!/usr/bin/env python3
"""A reproducible XNU bring-up bench; not a native Switch or finished iPadOS port."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from scripts.observe import QMP, memory_sample

ROOT = Path(__file__).resolve().parent
HAKIOS_URL = "https://github.com/oosexgang/hakios-xnu-kernel.git"
HAKIOS_REV = "88513176b1e96ecf0b764cd62c6f4e6ac71345e3"
HAKIOS_FILES = {
    "prebuilt/mach.development.bcm2837": "03e794e8e1d389a6abb9bcd2431eaccf9747271ff8c1234922225718c70441e9",
    "prebuilt/loader.efi": "3de0d8b2095989a9a8f9f9679b8bec01d80bad38eee535d934d29b7b76fee984",
    "prebuilt/userland/hello": "48a910940230506813aa0b75692e58d1c6698feccf7c6f7771c8790b6c97522d",
}
SMOKE_MARKER = b"Hello from user space on Darwin!"


class Error(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def contained(root, name):
    """Only regular files/directories inside the selected artifact bundle."""
    rel = Path(name)
    if rel.is_absolute() or ".." in rel.parts or not rel.parts:
        raise Error(f"Invalid bundle path: {name!r}")
    path = (root / rel).resolve()
    if not path.is_relative_to(root.resolve()):
        raise Error(f"Bundle path escapes its root: {name!r}")
    candidate = root
    for part in rel.parts:
        candidate = candidate / part
        if candidate.is_symlink():
            raise Error(f"Symlinks are not supported in bundle paths: {name!r}")
    return path


def firmware_path(value=None):
    candidates = [Path(value)] if value else [
        Path("/usr/share/AAVMF/AAVMF_CODE.fd"),
        Path("/usr/share/edk2/aarch64/QEMU_EFI.fd"),
        Path("/opt/homebrew/share/qemu/edk2-aarch64-code.fd"),
        Path("/usr/local/share/qemu/edk2-aarch64-code.fd"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise Error("AArch64 EDK2 firmware is missing; supply --firmware PATH (see README).")


def memory_available():
    path = Path("/proc/meminfo")
    if path.is_file():
        match = re.search(r"^MemAvailable:\s+(\d+) kB$", path.read_text(), re.M)
        if match:
            return int(match[1]) // 1024
    return None


def doctor():
    try:
        firmware = str(firmware_path())
    except Error:
        firmware = None
    model = Path("/proc/device-tree/model")
    return {
        "host": platform.system(), "architecture": platform.machine(),
        "model": model.read_text().rstrip("\0") if model.is_file() else None,
        "qemu": shutil.which("qemu-system-aarch64"), "firmware": firmware,
        "mem_available_mib": memory_available(),
        "switch_verified": False, "native_hekate_boot": False,
        "note": "TCG only. A successful host smoke test is not an iPadOS/Switch boot.",
    }


def fetch_hakios(destination):
    destination = Path(destination).resolve()
    if destination.exists():
        raise Error(f"Destination already exists: {destination}; use smoke --source to reuse it.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "--quiet", str(destination)], check=True)
    subprocess.run(["git", "-C", str(destination), "fetch", "--depth=1", HAKIOS_URL, HAKIOS_REV], check=True)
    subprocess.run(["git", "-C", str(destination), "checkout", "--detach", "FETCH_HEAD"], check=True)
    verify_hakios(destination)
    print(f"Pinned HakiOS source ready: {destination}")


def verify_hakios(source):
    for name, expected in HAKIOS_FILES.items():
        path = source / name
        if not path.is_file() or sha256(path) != expected:
            raise Error(f"Missing or changed pinned HakiOS artifact: {path}")


def payload(entries):
    """XNUP is the documented HakiOS loader protocol, not a Hekate payload."""
    body = bytearray()
    for name, data in entries:
        encoded = name.encode("ascii")
        if not 0 < len(encoded) < 16 or len(data) >= 2**32:
            raise Error("Invalid XNUP entry")
        body += struct.pack("<16sII", encoded, len(data), 0)
        body += data + b"\0" * (-len(data) % 16)
    body += b"\0" * 24
    if len(body) + 8 > 0x04000000:
        raise Error("Payload exceeds the HakiOS 64 MiB reservation")
    return struct.pack("<II", 0x50554E58, len(body)) + body


def new_run(base):
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=base))


def qemu_base(firmware, run, ram, cpu, display, fullscreen=False, tcg_cache_mib=128):
    # QEMU keyval arguments interpret commas. Reject them in local artifact paths.
    for path in (firmware, run):
        if "," in str(path) or "\n" in str(path):
            raise Error("QEMU artifact paths must not contain commas or newlines")
    command = ["qemu-system-aarch64", "-machine", "virt,virtualization=off,secure=off,gic-version=3",
               "-accel", f"tcg,tb-size={tcg_cache_mib}", "-cpu", cpu, "-smp", "1", "-m", str(ram),
               "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
               "-drive", f"if=pflash,format=raw,file={run / 'vars.fd'}",
               "-device", "ramfb", "-display", display, "-serial", "stdio",
               "-monitor", "none", "-nic", "none", "-no-reboot"]
    if fullscreen:
        command += ["-full-screen"]
    return command


def make_vars(run):
    # QEMU virt exposes two 64 MiB flash banks. A zeroed writable bank matches
    # UniXNU's run-qemu.py and is populated by EDK2 on first boot.
    with (run / "vars.fd").open("xb") as stream:
        stream.truncate(64 * 1024 * 1024)


def execute(command, run, seconds, smoke=False, capture_every=20):
    # macOS Unix socket paths are short. Keep only the socket in /tmp;
    # images, logs and measurements stay in the requested run directory.
    with tempfile.TemporaryDirectory(prefix="ioswitch-qmp-", dir="/tmp") as work:
        return _execute(command, run, seconds, smoke, capture_every, Path(work) / "qmp")


def _execute(command, run, seconds, smoke, capture_every, qmp_path):
    if not shutil.which(command[0]):
        raise Error("qemu-system-aarch64 is missing; see README for host dependencies")
    command = list(command)
    if not smoke:
        command += ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
    (run / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    print(f"Serial log: {run / 'serial.log'}", flush=True)
    started = time.monotonic()
    matched = False
    reason = "exited"
    observer = None
    next_sample = 0.0
    next_capture = float(capture_every)
    memory_max = {}
    captures = []
    observation_errors = []
    with (run / "serial.log").open("wb") as log:
        process = subprocess.Popen(command, cwd=run, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if elapsed >= next_sample:
                    sample = memory_sample(process.pid)
                    if sample:
                        with (run / "memory.jsonl").open("a") as samples:
                            samples.write(json.dumps({"elapsed_seconds": round(elapsed, 2), **sample}) + "\n")
                        for key, value in sample.items():
                            memory_max[key] = max(value, memory_max.get(key, 0))
                    next_sample = elapsed + 2
                if not smoke and elapsed >= next_capture:
                    try:
                        if observer is None:
                            observer = QMP(qmp_path)
                        frame = run / f"frame-{int(elapsed):06d}.png"
                        observer.screenshot(frame)
                        captures.append(frame.name)
                    except (OSError, ValueError) as exc:
                        observation_errors.append({"elapsed_seconds": round(elapsed, 2), "error": str(exc)})
                    finally:
                        # QEMU accepts one client on this socket. Release it between
                        # frames so input and guest-log tools can inspect the live VM.
                        if observer:
                            observer.close()
                            observer = None
                    next_capture = time.monotonic() - started + capture_every
                if smoke:
                    data = (run / "serial.log").read_bytes()
                    matched = SMOKE_MARKER in data and b"I am process 1, uid 0" in data
                    if matched:
                        reason = "userspace-confirmed"
                        break
                if time.monotonic() - started >= seconds:
                    reason = "timeout"
                    break
                time.sleep(0.25)
        except KeyboardInterrupt:
            reason = "interrupted"
        finally:
            if observer:
                observer.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if smoke:
        data = (run / "serial.log").read_bytes()
        matched = SMOKE_MARKER in data and b"I am process 1, uid 0" in data
    result = {"profile": "hakios-smoke" if smoke else "unixnu-ipados",
              "host": platform.system(), "architecture": platform.machine(),
              "elapsed_seconds": round(time.monotonic() - started, 2),
              "qemu_exit_code": process.returncode, "stop_reason": reason,
              "xnu_userspace_confirmed": matched if smoke else None,
              "ipad_ui_verified": False, "switch_verified": False,
              "observed_memory_max_kib": memory_max,
              "memory_sampling_seconds": 2,
              "frames": captures, "observation_errors": observation_errors}
    (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if smoke and not matched:
        print((run / "serial.log").read_text(errors="replace")[-6000:], file=sys.stderr)
        raise Error("XNU userspace marker not observed. See serial.log; this smoke test failed.")
    if not smoke and process.returncode not in (0, -15) and reason == "exited":
        raise Error("QEMU exited with an error. See serial.log.")
    return result


def smoke(args):
    source = Path(args.source).resolve()
    verify_hakios(source)
    firmware = firmware_path(args.firmware)
    run = new_run(args.runs)
    boot = run / "esp/EFI/BOOT"
    boot.mkdir(parents=True)
    shutil.copyfile(source / "prebuilt/loader.efi", boot / "BOOTAA64.EFI")
    entries = [("kernel", (source / "prebuilt/mach.development.bcm2837").read_bytes()),
               ("ramdisk", (source / "prebuilt/userland/hello").read_bytes()),
               ("cmdline", f"cpus=1 serial=3 loader_return_after={args.seconds} epoch={int(time.time())}".encode() + b"\0")]
    (run / "payload.bin").write_bytes(payload(entries))
    make_vars(run)
    command = qemu_base(firmware, run, 4096, args.cpu, "none")
    command += ["-drive", f"file=fat:ro:{run / 'esp'},format=raw,readonly=on,if=virtio",
                "-device", f"loader,file={run / 'payload.bin'},addr=0xF0000000,force-raw=on"]
    (run / "provenance.json").write_text(json.dumps({"repository": HAKIOS_URL, "revision": HAKIOS_REV,
                                                       "sha256": HAKIOS_FILES,
                                                       "test_cpu": args.cpu,
                                                       "gic": "emulated-v3"}, indent=2) + "\n")
    execute(command, run, args.seconds, smoke=True)


def load_bundle(directory):
    root = Path(directory).resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    if not isinstance(manifest, dict):
        raise Error("Bundle manifest must be an object")
    expected = {"schema_version": 1, "profile": "unixnu-ipados", "ram_mib": 6144,
                "cpu": "cortex-a710", "accel": "tcg", "smp": 1}
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise Error(f"Unsupported {key}: expected {value!r}")
    files = manifest.get("files", {})
    if not isinstance(files, dict) or not files:
        raise Error("Bundle has no file hashes")
    for name, metadata in files.items():
        if (not isinstance(metadata, dict) or not isinstance(metadata.get("size_bytes"), int)
                or metadata["size_bytes"] < 0 or not isinstance(metadata.get("sha256"), str)
                or not re.fullmatch(r"[0-9a-f]{64}", metadata["sha256"])):
            raise Error(f"Invalid file record: {name}")
        path = contained(root, name)
        if (not path.is_file() or path.stat().st_size != metadata["size_bytes"]
                or sha256(path) != metadata["sha256"]):
            raise Error(f"Bundle file is missing or changed: {name}")
    for key in ("firmware", "os_disk", "systemos_disk"):
        if manifest[key] not in files:
            raise Error(f"Unhashed {key}")
        contained(root, manifest[key])
    esp = contained(root, manifest["esp"])
    if not esp.is_dir() or not (esp / "EFI/BOOT/BOOTAA64.EFI").is_file():
        raise Error("Missing UEFI boot application")
    # QEMU reads the whole ESP. Ensure extra files cannot bypass the hash check.
    for path in esp.rglob("*"):
        if path.is_symlink():
            raise Error("Symlinks are not supported in the ESP")
        if not (path.is_dir() or path.is_file()):
            raise Error(f"Special files are not supported in the ESP: {path.name}")
        if path.is_file() and path.relative_to(root).as_posix() not in files:
            raise Error(f"Unhashed ESP file: {path.name}")
    screen = manifest.get("screen", "")
    if not re.fullmatch(r"\d+x\d+", screen):
        raise Error("Invalid screen size")
    width, height = map(int, screen.split("x"))
    if width % 16 or not (320 <= width <= 4096 and 320 <= height <= 4096):
        raise Error("Unsupported screen dimensions")
    for key in ("firmware", "esp", "os_disk", "systemos_disk"):
        if any(c in str(contained(root, manifest[key])) for c in (",", "\n")):
            raise Error("QEMU artifact paths must not contain commas or newlines")
    return root, manifest


def ipad_command(root, manifest, run, display, fullscreen=False, tcg_cache_mib=128):
    if display == "cocoa":
        display += ",zoom-to-fit=on,show-cursor=on"
    elif display == "gtk":
        display += ",zoom-to-fit=on"
    command = qemu_base(contained(root, manifest["firmware"]), run,
                        manifest["ram_mib"], manifest["cpu"], display, fullscreen,
                        tcg_cache_mib=tcg_cache_mib)
    # This topology matches UniXNU-qemu's root policy: ESP is PCI, OS disks MMIO.
    command += ["-drive", f"if=none,id=esp,format=raw,readonly=on,file=fat:ro:{root / manifest['esp']}",
                "-device", "virtio-blk-pci,drive=esp",
                "-device", "qemu-xhci", "-device", "usb-kbd",
                "-fw_cfg", f"name=opt/org.unixnu/ramfb-size,string={manifest['screen']}",
                "-global", "virtio-mmio.force-legacy=false"]
    for key, disk_id in (("os_disk", "osroot"), ("systemos_disk", "systemos")):
        command += ["-drive", f"if=none,id={disk_id},format=raw,readonly=on,file={root / manifest[key]}",
                    "-device", f"virtio-blk-device,drive={disk_id}"]
    command += ["-device", "virtio-tablet-device", "-device", "virtio-keyboard-device"]
    return command


def run_ipad(args):
    root, manifest = load_bundle(args.bundle)
    if Path(args.runs).resolve().is_relative_to(root):
        raise Error("Run output must be outside the imported bundle to preserve its verified files")
    if args.dry_run:
        print(json.dumps(ipad_command(root, manifest, root / "RUN_DIRECTORY", args.display, args.fullscreen,
                                      tcg_cache_mib=args.tcg_cache_mib), indent=2))
        return
    available = memory_available()
    if available is not None and available < 3072 and not args.allow_low_memory:
        raise Error(f"Only {available} MiB host memory available; experimental iPadOS boot requires at least "
                    "3072 MiB available. This is a preflight threshold, not a fit guarantee. "
                    "--allow-low-memory explicitly accepts the OOM risk; guest RAM cannot just be reduced.")
    run = new_run(args.runs)
    make_vars(run)
    shutil.copyfile(root / "manifest.json", run / "manifest.json")
    print("Experimental iPadOS boot: Switch UI, memory fit, and performance are unverified.")
    execute(ipad_command(root, manifest, run, args.display, args.fullscreen,
                         tcg_cache_mib=args.tcg_cache_mib), run, args.seconds,
            capture_every=args.capture_every)


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def tcg_cache_size(value):
    number = int(value)
    if not 16 <= number <= 1024:
        raise argparse.ArgumentTypeError("must be between 16 and 1024 MiB")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor", help="Report host readiness without changing it")
    fetch = sub.add_parser("fetch-hakios", help="Fetch pinned public source; no Apple OS images")
    fetch.add_argument("--directory", default=str(ROOT / "vendor/hakios"))
    test = sub.add_parser("smoke", help="Boot XNU + open test process in QEMU (not iOS)")
    test.add_argument("--source", default=str(ROOT / "vendor/hakios"))
    test.add_argument("--firmware")
    test.add_argument("--runs", default=str(ROOT / "out/runs"))
    test.add_argument("--seconds", type=positive, default=180)
    test.add_argument("--cpu", choices=("cortex-a76", "cortex-a57"), default="cortex-a76",
                      help="A57 is an ISA compatibility experiment; GIC remains emulated v3, not Switch hardware")
    run = sub.add_parser("run", help="Run an exported UniXNU iPadOS bundle in TCG")
    run.add_argument("--bundle", required=True)
    run.add_argument("--runs", default=str(ROOT / "out/runs"))
    run.add_argument("--seconds", type=positive, default=1200)
    run.add_argument("--tcg-cache-mib", type=tcg_cache_size, default=128,
                     help="TCG translation cache size in MiB, 16–1024 (default: 128)")
    run.add_argument("--capture-every", type=positive, default=20,
                     help="Save a lossless guest screenshot every N seconds; memory is sampled every 2 seconds")
    run.add_argument("--display", choices=("gtk", "sdl", "cocoa", "none"), default="cocoa" if sys.platform == "darwin" else "gtk")
    run.add_argument("--fullscreen", action="store_true")
    run.add_argument("--allow-low-memory", action="store_true")
    run.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "doctor":
            print(json.dumps(doctor(), indent=2))
        elif args.command == "fetch-hakios":
            fetch_hakios(args.directory)
        elif args.command == "smoke":
            smoke(args)
        else:
            run_ipad(args)
    except (Error, OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"ioswitch: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
