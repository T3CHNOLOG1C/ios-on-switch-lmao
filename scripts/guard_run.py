#!/usr/bin/env python3
"""Bound a Linux trial inside an already memory-limited service; never alter host policy.

The cgroup cap limits RAM, not RAM plus existing swap. SIGKILL/OOM of this guard
cannot run cleanup; the service owner must then check/stop the service. Children
that deliberately create another session also require service-level cleanup.
"""
import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import signal
import subprocess
import sys
import time

MEMORY_CEILING = 3 * 1024 ** 3
AVAILABLE_FLOOR = 384 * 1024 ** 2
THERMAL_CEILING = 80000


class GuardError(Exception):
    pass


def unescape_mount(value):
    return re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), value)


def cgroup_memory_limit(proc=Path("/proc")):
    """Resolve our real mount/path and the smallest enforced ancestor RAM limit."""
    memberships = {}
    for line in (proc / "self/cgroup").read_text().splitlines():
        _, controllers, path = line.split(":", 2)
        if "memory" in controllers.split(","):
            memberships["cgroup"] = path
        elif controllers == "":
            memberships["cgroup2"] = path
    for line in (proc / "self/mountinfo").read_text().splitlines():
        before, after = line.split(" - ", 1)
        fields, filesystem = before.split(), after.split()
        kind = filesystem[0]
        if kind not in memberships:
            continue
        if kind == "cgroup2" and "cgroup" in memberships:
            continue  # Hybrid hosts may keep the memory controller on v1.
        if kind == "cgroup" and "memory" not in filesystem[2].split(","):
            continue
        root = PurePosixPath(unescape_mount(fields[3]))
        member = PurePosixPath(memberships[kind])
        if not member.is_absolute() or ".." in member.parts:
            raise GuardError("Invalid memory cgroup path")
        try:
            relative = member.relative_to(root)
        except ValueError:
            continue
        mount = Path(unescape_mount(fields[4]))
        leaf = mount / str(relative)
        limits = []
        directory = leaf
        while True:
            # A true v2 root has no memory.max; a mounted subtree can have one.
            if kind == "cgroup" or directory != mount or (directory / "memory.max").exists():
                enforced = directory == leaf or kind == "cgroup2"
                if not enforced:
                    hierarchy = (directory / "memory.use_hierarchy").read_text().strip()
                    if hierarchy not in {"0", "1"}:
                        raise GuardError("Invalid cgroup memory hierarchy setting")
                    enforced = hierarchy == "1"
                if enforced:
                    name = "memory.max" if kind == "cgroup2" else "memory.limit_in_bytes"
                    value = (directory / name).read_text().strip()
                    if value != "max":
                        if not value.isdigit() or int(value) <= 0:
                            raise GuardError("Invalid cgroup memory limit")
                        limits.append(int(value))
            if directory == mount:
                break
            directory = directory.parent
        effective = min(limits) if limits else None
        if effective is None or effective > MEMORY_CEILING:
            raise GuardError("Require an enforced cgroup RAM limit of at most 3 GiB")
        return {"version": 2 if kind == "cgroup2" else 1,
                "path": str(leaf), "effective_limit_bytes": effective}
    raise GuardError("Cannot resolve this process's memory cgroup")


def sample_host(proc=Path("/proc"), sysfs=Path("/sys")):
    match = re.search(r"^MemAvailable:\s+(\d+) kB$", (proc / "meminfo").read_text(), re.M)
    if not match:
        raise GuardError("MemAvailable is missing or malformed")
    available = int(match[1]) * 1024
    zones = sorted((sysfs / "class/thermal").glob("thermal_zone*"))
    if not zones:
        raise GuardError("No thermal zones are readable")
    temperatures = {zone.name: int((zone / "temp").read_text().strip()) for zone in zones}
    if any(not -100000 <= value <= 300000 for value in temperatures.values()):
        raise GuardError("Thermal reading is outside plausible millidegree bounds")
    batteries = []
    for supply in sorted((sysfs / "class/power_supply").glob("*")):
        if (supply / "type").read_text().strip() != "Battery":
            continue
        scope = supply / "scope"
        if scope.exists() and scope.read_text().strip() == "Device":
            continue  # Joy-Con/peripheral batteries do not power the host.
        capacity = int((supply / "capacity").read_text().strip())
        status = (supply / "status").read_text().strip()
        if not 0 <= capacity <= 100 or status not in {
                "Charging", "Discharging", "Not charging", "Full", "Unknown"}:
            raise GuardError("Battery reading is malformed")
        batteries.append({"name": supply.name, "capacity": capacity, "status": status})
    hazards = []
    if available < AVAILABLE_FLOOR:
        hazards.append("low_available_memory")
    if max(temperatures.values()) >= THERMAL_CEILING:
        hazards.append("thermal_limit")
    if any(b["capacity"] <= 5 and b["status"] in {"Discharging", "Unknown"} for b in batteries):
        hazards.append("low_battery")
    return {"available_bytes": available, "thermal_millicelsius": temperatures,
            "max_temperature_millicelsius": max(temperatures.values()),
            "batteries": batteries, "hazards": hazards}


def group_exists(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


@contextmanager
def defer_termination(raise_after):
    """Keep launch/assignment atomic to signals, and protect bounded cleanup.

    Catching (rather than blocking/ignoring) during launch means exec resets the
    child's handlers normally without inheriting an uninterruptible signal mask.
    """
    pending = []
    previous = {}
    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, lambda signum, frame: pending.append(signum))
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    if raise_after and pending:
        if signal.SIGINT in pending:
            raise KeyboardInterrupt
        raise InterruptedError(f"Received signal {pending[0]}")


def stop_group(process, grace=5):
    """Always address the saved group, including after its leader has exited."""
    pgid = process.pid  # Popen(start_new_session=True) makes this the child PGID.
    if pgid == os.getpgrp():
        raise GuardError("Refusing to signal the guard's own process group")
    sent = []

    def alive():
        process.poll()
        try:
            return group_exists(pgid)
        except PermissionError:
            # Darwin can report EPERM for a group whose dead leader is unreaped.
            # A live/unsignalable leader still fails this bounded cleanup.
            process.wait(timeout=0.1)
            return group_exists(pgid)

    def send(sig):
        try:
            try:
                os.killpg(pgid, sig)
            except PermissionError:
                process.wait(timeout=0.1)
                os.killpg(pgid, sig)
            sent.append(signal.Signals(sig).name)
        except ProcessLookupError:
            pass

    with defer_termination(raise_after=False):
        try:
            if alive():
                send(signal.SIGINT)
                deadline = time.monotonic() + grace
                while alive() and time.monotonic() < deadline:
                    time.sleep(0.05)
        finally:
            try:
                if alive():
                    send(signal.SIGKILL)
            finally:
                process.wait(timeout=5)
    return sent


def run(command, output, seconds):
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    process = None
    summary = {"command": command, "seconds_limit": seconds, "samples": 0,
               "thresholds": {"cgroup_ram_ceiling_bytes": MEMORY_CEILING,
                              "available_floor_bytes": AVAILABLE_FLOOR,
                              "thermal_ceiling_millicelsius": THERMAL_CEILING},
               "stop_reason": "error", "exit_code": 2}
    with (output / "guard.jsonl").open("x", buffering=1) as events, (output / "child.log").open("xb") as child_log:
        def record(event, **values):
            events.write(json.dumps({"event": event, "elapsed_seconds": round(time.monotonic() - started, 3),
                                     **values}) + "\n")

        def sample():
            reading = sample_host()
            summary["samples"] += 1
            record("sample", **reading)
            return reading["hazards"]

        try:
            summary["cgroup"] = cgroup_memory_limit()
            record("preflight", command=command, cgroup=summary["cgroup"], thresholds=summary["thresholds"])
            hazards = sample()
            if hazards:
                summary.update(stop_reason="guard_limit", hazards=hazards, exit_code=3)
            else:
                with defer_termination(raise_after=True):
                    process = subprocess.Popen(command, start_new_session=True,
                                               stdin=subprocess.DEVNULL, stdout=child_log, stderr=subprocess.STDOUT)
                    summary["child_pid"] = process.pid
                record("child_started", pid=process.pid)
                deadline = time.monotonic() + seconds
                while process.poll() is None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        summary.update(stop_reason="deadline", exit_code=124)
                        break
                    hazards = sample()
                    if hazards:
                        summary.update(stop_reason="guard_limit", hazards=hazards, exit_code=3)
                        break
                    time.sleep(min(1, remaining))
                else:
                    summary.update(stop_reason="child_exit", exit_code=0 if process.returncode == 0 else 1)
        except KeyboardInterrupt:
            summary.update(stop_reason="interrupted", exit_code=130)
        except InterruptedError as exc:
            summary.update(stop_reason="terminated", error=str(exc), exit_code=143)
        except Exception as exc:
            summary.update(stop_reason="error", error=f"{type(exc).__name__}: {exc}", exit_code=2)
        finally:
            if process is not None:
                try:
                    summary["cleanup_signals"] = stop_group(process)
                    if summary["exit_code"] == 0 and summary["cleanup_signals"]:
                        summary.update(stop_reason="remaining_process_group", exit_code=1)
                except BaseException as exc:
                    summary.update(cleanup_error=f"{type(exc).__name__}: {exc}", exit_code=2)
                summary["child_returncode"] = process.returncode
            summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
            with (output / "summary.json").open("x") as stream:
                json.dump(summary, stream, indent=2)
                stream.write("\n")
            record("finished", **summary)
    return summary["exit_code"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("Supply a command after -- and a positive finite --seconds value")

    def terminate(signum, frame):
        raise InterruptedError(f"Received signal {signum}")

    signal.signal(signal.SIGTERM, terminate)
    try:
        return run(command, args.output, args.seconds)
    except Exception as exc:
        print(f"guard: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
