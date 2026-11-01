import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from scripts import guard_run as guard


class FixtureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.proc = self.base / "proc"
        self.sysfs = self.base / "sys"
        (self.proc / "self").mkdir(parents=True)

    def write(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(str(value))

    def cgroup(self, version=1, root="/", member="/service", limit=guard.MEMORY_CEILING):
        mount = self.base / "memory mount"
        leaf = mount / str(Path(member).relative_to(root))
        escaped = str(mount).replace(" ", r"\040")
        if version == 1:
            self.write(self.proc / "self/cgroup", f"4:cpu:/unused\n7:memory:{member}\n")
            self.write(self.proc / "self/mountinfo", f"1 0 0:1 {root} {escaped} rw - cgroup cgroup rw,memory\n")
            self.write(mount / "memory.limit_in_bytes", 9223372036854771712)
            self.write(mount / "memory.use_hierarchy", 0)
            self.write(leaf / "memory.limit_in_bytes", limit)
        else:
            self.write(self.proc / "self/cgroup", f"0::{member}\n")
            self.write(self.proc / "self/mountinfo", f"1 0 0:1 {root} {escaped} rw - cgroup2 cgroup rw\n")
            self.write(leaf / "memory.max", limit)
        return mount, leaf

    def sensors(self):
        self.write(self.proc / "meminfo", "MemAvailable: 524288 kB\n")
        self.write(self.sysfs / "class/thermal/thermal_zone0/temp", 45000)
        self.write(self.sysfs / "class/thermal/thermal_zone1/temp", 50000)

    def test_v1_resolves_actual_membership_mount_root_and_escaped_mountpoint(self):
        _, leaf = self.cgroup(root="/host", member="/host/service")
        result = guard.cgroup_memory_limit(self.proc)
        self.assertEqual(result["path"], str(leaf))
        self.assertEqual(result["effective_limit_bytes"], guard.MEMORY_CEILING)
        self.write(self.proc / "self/cgroup", "7:memory:/hostname/service\n")
        with self.assertRaises(guard.GuardError):
            guard.cgroup_memory_limit(self.proc)

    def test_oversized_unlimited_missing_and_malformed_limits_fail_closed(self):
        _, leaf = self.cgroup()
        for limit in (guard.MEMORY_CEILING + 1, 9223372036854771712, "max", "NaN", 0):
            with self.subTest(limit=limit):
                self.write(leaf / "memory.limit_in_bytes", limit)
                with self.assertRaises(guard.GuardError):
                    guard.cgroup_memory_limit(self.proc)
        (leaf / "memory.limit_in_bytes").unlink()
        with self.assertRaises(OSError):
            guard.cgroup_memory_limit(self.proc)

    def test_v1_parent_limit_only_counts_when_hierarchy_is_enabled(self):
        mount, _ = self.cgroup(member="/parent/service", limit="max")
        self.write(mount / "parent/memory.limit_in_bytes", 2 * 1024 ** 3)
        self.write(mount / "parent/memory.use_hierarchy", 1)
        self.assertEqual(guard.cgroup_memory_limit(self.proc)["effective_limit_bytes"], 2 * 1024 ** 3)
        self.write(mount / "parent/memory.use_hierarchy", 0)
        with self.assertRaises(guard.GuardError):
            guard.cgroup_memory_limit(self.proc)

    def test_v2_inherited_limit_and_unlimited_root(self):
        mount, _ = self.cgroup(version=2, member="/parent/service", limit="max")
        self.write(mount / "parent/memory.max", guard.MEMORY_CEILING)
        self.assertEqual(guard.cgroup_memory_limit(self.proc)["version"], 2)
        self.write(mount / "parent/memory.max", "max")
        with self.assertRaises(guard.GuardError):
            guard.cgroup_memory_limit(self.proc)

    def test_v2_mounted_subtree_keeps_its_limit(self):
        self.cgroup(version=2, root="/service", member="/service")
        self.assertEqual(guard.cgroup_memory_limit(self.proc)["effective_limit_bytes"], guard.MEMORY_CEILING)

    def test_memory_thermal_and_battery_hazards_have_exact_boundaries(self):
        self.sensors()
        self.assertEqual(guard.sample_host(self.proc, self.sysfs)["hazards"], [])
        self.write(self.proc / "meminfo", "MemAvailable: 393215 kB\n")
        self.write(self.sysfs / "class/thermal/thermal_zone1/temp", 80000)
        battery = self.sysfs / "class/power_supply/BAT0"
        self.write(battery / "type", "Battery")
        self.write(battery / "capacity", 5)
        self.write(battery / "status", "Discharging")
        self.assertEqual(guard.sample_host(self.proc, self.sysfs)["hazards"],
                         ["low_available_memory", "thermal_limit", "low_battery"])
        self.write(self.proc / "meminfo", "MemAvailable: 393216 kB\n")
        self.write(self.sysfs / "class/thermal/thermal_zone1/temp", 79999)
        self.write(battery / "status", "Charging")
        self.assertEqual(guard.sample_host(self.proc, self.sysfs)["hazards"], [])
        (battery / "capacity").unlink()
        with self.assertRaises(OSError):
            guard.sample_host(self.proc, self.sysfs)

    def test_peripheral_battery_without_capacity_is_not_a_host_battery(self):
        self.sensors()
        battery = self.sysfs / "class/power_supply/joycon"
        self.write(battery / "type", "Battery")
        self.write(battery / "scope", "Device")
        self.assertEqual(guard.sample_host(self.proc, self.sysfs)["batteries"], [])
        self.write(battery / "scope", "System")
        with self.assertRaises(OSError):
            guard.sample_host(self.proc, self.sysfs)

    def test_missing_or_bad_required_sensor_is_not_a_safe_reading(self):
        self.sensors()
        for bad in ("NaN", "", "-999999"):
            self.write(self.sysfs / "class/thermal/thermal_zone0/temp", bad)
            with self.assertRaises((ValueError, guard.GuardError)):
                guard.sample_host(self.proc, self.sysfs)
        (self.sysfs / "class/thermal/thermal_zone0/temp").unlink()
        with self.assertRaises(OSError):
            guard.sample_host(self.proc, self.sysfs)
        self.sensors()
        self.write(self.proc / "meminfo", "MemFree: 524288 kB\n")
        with self.assertRaises(guard.GuardError):
            guard.sample_host(self.proc, self.sysfs)

    def test_invalid_preflight_never_starts_child_and_records_failure(self):
        output = self.base / "trial"
        with patch.object(guard, "cgroup_memory_limit", side_effect=guard.GuardError("uncapped")), \
                patch.object(guard.subprocess, "Popen") as popen:
            self.assertEqual(guard.run(["must-not-run"], output, 1), 2)
            popen.assert_not_called()
        self.assertIn("uncapped", json.loads((output / "summary.json").read_text())["error"])
        with self.assertRaises(FileExistsError):
            guard.run(["must-not-run"], output, 1)


@unittest.skipUnless(os.name == "posix", "Process groups require POSIX")
class ProcessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.cap = patch.object(guard, "cgroup_memory_limit", return_value={"effective_limit_bytes": guard.MEMORY_CEILING})
        self.cap.start()
        self.addCleanup(self.cap.stop)

    def wait_file(self, path):
        until = time.monotonic() + 3
        while not path.exists() and time.monotonic() < until:
            time.sleep(0.01)
        self.assertTrue(path.exists(), "child failed to become ready")

    def test_stubborn_child_is_killed_and_reaped(self):
        ready = self.base / "ready"
        code = "import pathlib,signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); pathlib.Path(__import__('sys').argv[1]).touch(); time.sleep(60)"
        process = subprocess.Popen([sys.executable, "-c", code, str(ready)], start_new_session=True)
        try:
            self.wait_file(ready)
            self.assertEqual(guard.stop_group(process, grace=0.05), ["SIGINT", "SIGKILL"])
            self.assertEqual(process.returncode, -signal.SIGKILL)
        finally:
            guard.stop_group(process, grace=0)

    def test_cleanup_signals_descendant_after_leader_exits(self):
        heartbeat = self.base / "heartbeat"
        child = "import pathlib,signal,time,sys; signal.signal(signal.SIGINT,signal.SIG_IGN); p=pathlib.Path(sys.argv[1]);\nwhile True: p.write_text(str(time.monotonic())); time.sleep(.02)"
        leader = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]])"
        process = subprocess.Popen([sys.executable, "-c", leader, child, str(heartbeat)], start_new_session=True)
        try:
            process.wait(timeout=3)
            self.wait_file(heartbeat)
            self.assertIn("SIGKILL", guard.stop_group(process, grace=0.05))
            time.sleep(0.05)
            previous = heartbeat.read_text()
            time.sleep(0.08)
            self.assertEqual(heartbeat.read_text(), previous)
        finally:
            guard.stop_group(process, grace=0)

    def test_monitor_failures_hazards_interrupt_and_deadline_all_cleanup(self):
        safe = {"hazards": []}
        cases = [(OSError("thermal disappeared"), 2), (KeyboardInterrupt(), 130),
                 ({"hazards": ["thermal_limit"]}, 3), (None, 124)]
        original_stop = guard.stop_group
        for index, (failure, expected) in enumerate(cases):
            output = self.base / f"trial-{index}"
            readings = [safe, failure] if failure is not None else None
            with self.subTest(failure=failure), \
                    patch.object(guard, "sample_host", side_effect=readings, return_value=safe), \
                    patch.object(guard, "stop_group", side_effect=lambda process: original_stop(process, grace=0.05)):
                result = guard.run([sys.executable, "-c", "import time; time.sleep(60)"], output, 0.05)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(result, expected)
            self.assertIsNotNone(summary["child_returncode"])
            self.assertFalse(guard.group_exists(summary["child_pid"]))

    def test_child_nonzero_is_failure(self):
        with patch.object(guard, "sample_host", return_value={"hazards": []}):
            self.assertEqual(guard.run([sys.executable, "-c", "raise SystemExit(7)"], self.base / "trial", 3), 1)

    def test_signal_during_launch_is_deferred_until_child_can_be_cleaned(self):
        real_popen = subprocess.Popen

        def launch(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            os.kill(os.getpid(), signal.SIGINT)
            return process

        output = self.base / "trial"
        with patch.object(guard, "sample_host", return_value={"hazards": []}), \
                patch.object(guard.subprocess, "Popen", side_effect=launch):
            self.assertEqual(guard.run([sys.executable, "-c", "import time; time.sleep(60)"], output, 3), 130)
        summary = json.loads((output / "summary.json").read_text())
        self.assertIsNotNone(summary["child_returncode"])
        self.assertFalse(guard.group_exists(summary["child_pid"]))

    def test_repeated_termination_during_cleanup_cannot_skip_kill(self):
        ready = self.base / "ready"
        code = "import pathlib,signal,time,sys; signal.signal(signal.SIGINT,signal.SIG_IGN); pathlib.Path(sys.argv[1]).touch(); time.sleep(60)"
        process = subprocess.Popen([sys.executable, "-c", code, str(ready)], start_new_session=True)
        real_sleep = time.sleep

        def interrupt_sleep(delay):
            os.kill(os.getpid(), signal.SIGINT)
            os.kill(os.getpid(), signal.SIGTERM)
            real_sleep(delay)

        try:
            self.wait_file(ready)
            with patch.object(guard.time, "sleep", side_effect=interrupt_sleep):
                self.assertEqual(guard.stop_group(process, grace=0.05), ["SIGINT", "SIGKILL"])
            self.assertEqual(process.returncode, -signal.SIGKILL)
        finally:
            guard.stop_group(process, grace=0)

    def test_zero_exit_with_lingering_descendants_is_failure(self):
        child = "import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); time.sleep(60)"
        leader = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',sys.argv[1]])"
        original_stop = guard.stop_group
        output = self.base / "trial"
        with patch.object(guard, "sample_host", return_value={"hazards": []}), \
                patch.object(guard, "stop_group", side_effect=lambda process: original_stop(process, grace=0.05)):
            self.assertEqual(guard.run([sys.executable, "-c", leader, child], output, 3), 1)
        summary = json.loads((output / "summary.json").read_text())
        self.assertEqual(summary["stop_reason"], "remaining_process_group")
        self.assertEqual(summary["child_returncode"], 0)
        self.assertIn("SIGKILL", summary["cleanup_signals"])


if __name__ == "__main__":
    unittest.main()
