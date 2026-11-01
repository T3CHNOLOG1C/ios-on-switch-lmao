import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_preflight.py"
SPEC = importlib.util.spec_from_file_location("build_preflight", SCRIPT)
preflight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(preflight)


class BuildPreflightTests(unittest.TestCase):
    def test_missing_scratch_is_not_created(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing"
            with self.assertRaises(ValueError):
                preflight.storage_check(missing, 50)
            self.assertFalse(missing.exists())

    def test_capacity_is_checked_on_resolved_selected_filesystem(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for free, enough in ((50 * preflight.GIB - 1, False), (50 * preflight.GIB, True)):
                with patch.object(preflight.shutil, "disk_usage", return_value=SimpleNamespace(free=free)) as usage:
                    report = preflight.storage_check(path, 50)
                    self.assertEqual(report["ok"], enough)
                    usage.assert_called_once_with(path.resolve())

    def test_budget_cannot_be_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            for invalid in (0, -1):
                with self.assertRaises(ValueError):
                    preflight.storage_check(Path(directory), invalid)

    def test_missing_dependencies_fail_without_running_or_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            args = SimpleNamespace(scratch=path, minimum_free_gib=50, openssl_prefix=path,
                                   uefi=None, unixnu=None, device=None)
            with patch.object(preflight, "find_tool", return_value=None), \
                    patch.object(preflight, "query") as query:
                report = preflight.inspect(args)
            query.assert_not_called()
            self.assertFalse(report["host_checks_passed"])
            self.assertFalse(report["build_or_boot_verified"])
            self.assertEqual(list(path.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
