#!/usr/bin/env python3
"""Start a bounded IOSwitch trial as the desktop user in a transient systemd service."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import pwd
import re
import secrets
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ioswitch import tcg_cache_size
from scripts.guard_run import defer_termination


def desktop_environment(display):
    if display == "none":
        return {}
    names = ("DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR")

    def local_session(values):
        # An SSH DISPLAY and a desktop XAUTHORITY are not a usable pair. Keep
        # each source intact and require the physical console's local X server.
        if not re.fullmatch(r"(?:unix)?:\d+(?:\.\d+)?", values.get("DISPLAY", "")):
            return None
        authority = values.get("XAUTHORITY")
        if not authority:
            return None
        authority = Path(authority).resolve()
        if not authority.is_file() or not os.access(authority, os.R_OK):
            return None
        return {**values, "XAUTHORITY": str(authority)}

    inherited = {name: os.environ[name] for name in names if os.environ.get(name)}
    selected = local_session(inherited)
    if selected:
        return selected
    result = subprocess.run(["systemctl", "--user", "show-environment"],
                            capture_output=True, text=True, check=True, timeout=10)
    managed = {}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name in names and value:
            managed[name] = value
    selected = local_session(managed)
    if not selected:
        raise ValueError("Open the Linux desktop first; a local DISPLAY and its readable XAUTHORITY are required")
    return selected


def submit_service(command, unit, trial, receipt):
    """A failed client submission does not prove that systemd created no unit."""
    receipt_path = trial / "launch.json"
    result = None
    try:
        # Delay SIGINT/SIGTERM until the bounded client call returns, so a
        # signal cannot skip ownership/cleanup of a successfully created unit.
        with defer_termination(raise_after=True):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            (trial / "service-start.log").write_text(result.stdout + result.stderr)
            if result.returncode:
                raise ValueError(f"systemd-run exited with status {result.returncode}")
            receipt.update(start_status="submitted", systemd_run_exit_code=0)
            receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    except (OSError, ValueError, subprocess.SubprocessError, KeyboardInterrupt) as exc:
        receipt.update(start_status="failed" if result is not None and result.returncode else "unknown",
                       submission_error=f"{type(exc).__name__}: {exc}")
        if result is not None:
            receipt["systemd_run_exit_code"] = result.returncode
        # Stop only the freshly named unit. A client timeout can occur after
        # the service has started, and the service outlives systemd-run itself.
        with defer_termination(raise_after=False):
            try:
                stopped = subprocess.run(["sudo", "-n", "systemctl", "stop", unit],
                                         capture_output=True, text=True, timeout=15)
                receipt["cleanup_stop_exit_code"] = stopped.returncode
                (trial / "service-stop.log").write_text(stopped.stdout + stopped.stderr)
            except (OSError, subprocess.SubprocessError) as cleanup_exc:
                receipt["cleanup_error"] = f"{type(cleanup_exc).__name__}: {cleanup_exc}"
            try:
                receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
            except OSError:
                pass  # The explicit unit in the error still permits cleanup.
        stop = shlex.join(["sudo", "systemctl", "stop", unit])
        raise ValueError(f"Service submission failed or was interrupted; cleanup was attempted. "
                         f"See {receipt_path}. If cleanup is unconfirmed, run: {stop}") from exc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--display", choices=("gtk", "none"), default="gtk")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--tcg-cache-mib", type=tcg_cache_size, default=128)
    args = parser.parse_args(argv)
    if not 30 <= args.seconds <= 3600:
        parser.error("--seconds must be between 30 and 3600")
    try:
        if platform.system() != "Linux" or os.getuid() == 0:
            raise ValueError("Run this from your normal user account on Switchroot Linux")
        for program in ("sudo", "systemd-run", "systemctl", "qemu-system-aarch64"):
            if not shutil.which(program):
                raise ValueError(f"Required program is missing: {program}")
        # A memcg OOM can panic the entire host with this kernel setting.
        if int(Path("/proc/sys/vm/panic_on_oom").read_text()) == 2:
            raise ValueError("Refusing a memory-limited trial while panic_on_oom is 2")
        bundle = args.bundle.resolve()
        if not (bundle / "manifest.json").is_file():
            raise ValueError("The complete exported bundle is required")
        trial_root = (ROOT / "out/trials").resolve()
        if trial_root.is_relative_to(bundle):
            raise ValueError("Trial output must be outside the bundle")
        environment = desktop_environment(args.display)
        user = pwd.getpwuid(os.getuid()).pw_name
        suffix = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3)
        unit = "ioswitch-trial-" + suffix + ".service"
        trial = trial_root / suffix
        trial.mkdir(parents=True, mode=0o700, exist_ok=False)
        guest = [sys.executable, "-u", str(ROOT / "ioswitch.py"), "run", "--bundle", str(bundle),
                 "--runs", str(trial / "runs"), "--seconds", str(args.seconds),
                 "--display", args.display, "--tcg-cache-mib", str(args.tcg_cache_mib),
                 "--allow-low-memory"]
        if args.fullscreen:
            guest.append("--fullscreen")
        command = ["sudo", "-n", "systemd-run", "--unit=" + unit, "--service-type=exec",
                   "--expand-environment=no",
                   "--uid=" + user, "--gid=" + str(os.getgid()), "--working-directory=" + str(ROOT),
                   "--property=MemoryMax=3G", "--property=MemoryAccounting=yes",
                   "--property=OOMPolicy=stop", "--property=OOMScoreAdjust=500", "--property=Nice=10",
                   "--property=RuntimeMaxSec=" + str(args.seconds + 30), "--property=TimeoutStopSec=8",
                   "--property=KillMode=control-group", "--property=UMask=0077"]
        command += ["--setenv=" + name + "=" + value for name, value in environment.items()]
        command += [sys.executable, "-u", str(ROOT / "scripts/guard_run.py"),
                    "--output", str(trial / "guard"), "--seconds", str(args.seconds + 10), "--", *guest]
        receipt = {"unit": unit, "command": command, "trial": str(trial),
                   "start_status": "prepared", "guest_boot_verified": False,
                   "note": "The guest rechecks all bundle hashes before QEMU starts. The guard checks the actual cgroup cap and host sensors."}
        receipt_path = trial / "launch.json"
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        submit_service(command, unit, trial, receipt)
        print(f"Trial submitted: {trial}")
        print("Stop this trial: " + shlex.join(["sudo", "systemctl", "stop", unit]))
        print("A running process is not a verified iPadOS boot; inspect the recorded frames and input response.")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"switch trial: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
