import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from scripts import switch_trial as trial


class DesktopEnvironmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.authority = self.base / "desktop-auth"
        self.authority.touch()
        self.manager = subprocess.CompletedProcess([], 0,
            f"DISPLAY=:0\nXAUTHORITY={self.authority}\nXDG_RUNTIME_DIR=/run/user/1000\n", "")

    def test_headless_needs_no_desktop(self):
        with patch.object(trial.subprocess, "run") as run:
            self.assertEqual(trial.desktop_environment("none"), {})
            run.assert_not_called()

    def test_forwarded_ssh_display_is_replaced_by_complete_desktop_pair(self):
        ssh_authority = self.base / "ssh-auth"
        ssh_authority.touch()
        with patch.dict(os.environ, {"DISPLAY": "localhost:10.0", "XAUTHORITY": str(ssh_authority)}, clear=True), \
                patch.object(trial.subprocess, "run", return_value=self.manager):
            result = trial.desktop_environment("gtk")
        self.assertEqual(result["DISPLAY"], ":0")
        self.assertEqual(result["XAUTHORITY"], str(self.authority))

    def test_partial_inherited_environment_does_not_mix_desktops(self):
        with patch.dict(os.environ, {"DISPLAY": ":1"}, clear=True), \
                patch.object(trial.subprocess, "run", return_value=self.manager):
            result = trial.desktop_environment("gtk")
        self.assertEqual(result["DISPLAY"], ":0")
        self.assertEqual(result["XAUTHORITY"], str(self.authority))

    def test_complete_local_environment_needs_no_manager(self):
        inherited = {"DISPLAY": ":0.0", "XAUTHORITY": str(self.authority)}
        with patch.dict(os.environ, inherited, clear=True), patch.object(trial.subprocess, "run") as run:
            self.assertEqual(trial.desktop_environment("gtk"), inherited)
            run.assert_not_called()

    def test_remote_display_or_directory_authority_is_rejected(self):
        for display, authority in (("localhost:10.0", self.authority), (":0", self.base)):
            manager = subprocess.CompletedProcess([], 0, f"DISPLAY={display}\nXAUTHORITY={authority}\n", "")
            with self.subTest(display=display), patch.dict(os.environ, {}, clear=True), \
                    patch.object(trial.subprocess, "run", return_value=manager), self.assertRaises(ValueError):
                trial.desktop_environment("gtk")


class SubmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.unit = "ioswitch-trial-test.service"
        self.command = ["sudo", "-n", "systemd-run", "--unit=" + self.unit, "test-command"]
        self.receipt = {"unit": self.unit, "start_status": "prepared"}
        self.ok = subprocess.CompletedProcess([], 0, "", "")

    def saved(self):
        return json.loads((self.base / "launch.json").read_text())

    def assert_stopped(self, run):
        self.assertEqual(run.call_args_list[-1].args[0], ["sudo", "-n", "systemctl", "stop", self.unit])
        self.assertEqual(run.call_args_list[-1].kwargs["timeout"], 15)

    def test_success_remains_running_and_records_submission(self):
        with patch.object(trial.subprocess, "run", return_value=self.ok) as run:
            trial.submit_service(self.command, self.unit, self.base, self.receipt)
            self.assertEqual(run.call_count, 1)
        self.assertEqual(self.saved()["start_status"], "submitted")

    def test_timeout_stops_possibly_created_service_and_records_uncertainty(self):
        with patch.object(trial.subprocess, "run",
                          side_effect=[subprocess.TimeoutExpired(self.command, 30), self.ok]) as run, \
                self.assertRaisesRegex(ValueError, self.unit):
            trial.submit_service(self.command, self.unit, self.base, self.receipt)
        self.assert_stopped(run)
        self.assertEqual(self.saved()["start_status"], "unknown")
        self.assertEqual(self.saved()["cleanup_stop_exit_code"], 0)

    def test_nonzero_submission_also_cleans_up(self):
        failed = subprocess.CompletedProcess([], 7, "", "failed to start")
        with patch.object(trial.subprocess, "run", side_effect=[failed, self.ok]) as run, \
                self.assertRaises(ValueError):
            trial.submit_service(self.command, self.unit, self.base, self.receipt)
        self.assert_stopped(run)
        self.assertEqual(self.saved()["start_status"], "failed")
        self.assertEqual(self.saved()["systemd_run_exit_code"], 7)

    def test_termination_during_submission_cannot_leave_created_service(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            calls = []

            def invoke(command, **kwargs):
                calls.append(command)
                if command == self.command:
                    os.kill(os.getpid(), signum)
                return self.ok

            with self.subTest(signal=signum), patch.object(trial.subprocess, "run", side_effect=invoke) as run, \
                    self.assertRaises(ValueError):
                trial.submit_service(self.command, self.unit, self.base, self.receipt)
            self.assert_stopped(run)
            self.assertEqual(len(calls), 2)
            self.assertEqual(self.saved()["start_status"], "unknown")

    def test_cleanup_timeout_is_explicit_in_receipt(self):
        with patch.object(trial.subprocess, "run", side_effect=[
                subprocess.TimeoutExpired(self.command, 30), subprocess.TimeoutExpired("stop", 15)]), \
                self.assertRaisesRegex(ValueError, "cleanup is unconfirmed"):
            trial.submit_service(self.command, self.unit, self.base, self.receipt)
        self.assertIn("TimeoutExpired", self.saved()["cleanup_error"])

    def test_command_keeps_literal_bundle_path_and_nested_deadlines(self):
        bundle = self.base / "bundle-$literal"
        bundle.mkdir()
        (bundle / "manifest.json").write_text("{}")
        with patch.object(trial, "ROOT", self.base), \
                patch.object(trial.platform, "system", return_value="Linux"), \
                patch.object(trial.os, "getuid", return_value=1000), \
                patch.object(trial.os, "getgid", return_value=1000), \
                patch.object(trial.pwd, "getpwuid", return_value=SimpleNamespace(pw_name="testuser")), \
                patch.object(trial.shutil, "which", return_value="/usr/bin/available"), \
                patch.object(trial.Path, "read_text", return_value="0"), \
                patch.object(trial, "submit_service") as submit:
            self.assertEqual(trial.main(["--bundle", str(bundle), "--seconds", "30", "--display", "none"]), 0)
        command = submit.call_args.args[0]
        self.assertIn(str(bundle), command)
        self.assertIn("--expand-environment=no", command)
        self.assertIn("--property=RuntimeMaxSec=60", command)
        self.assertIn("--property=KillMode=control-group", command)
        guard_args = command[command.index("--output"):]
        self.assertEqual(guard_args[guard_args.index("--seconds") + 1], "40")


if __name__ == "__main__":
    unittest.main()
