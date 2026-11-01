"""Runtime integration contracts; synthetic bundles are never boot evidence."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ioswitch
import test_export_unixnu as fixtures


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        # Reuse just the artifact fixture, without inheriting its test cases.
        self.fixture = fixtures.ExportTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.export()
        self.root = self.fixture.output

    def test_exporter_and_runtime_agree_on_bundle_contract(self):
        root, manifest = ioswitch.load_bundle(self.root)
        command = ioswitch.ipad_command(root, manifest, self.root.parent / "run", "gtk", True)
        devices = [command[i + 1] for i, value in enumerate(command[:-1]) if value == "-device"]
        self.assertEqual(devices, ["ramfb", "virtio-blk-pci,drive=esp", "qemu-xhci", "usb-kbd",
                                   "virtio-blk-device,drive=osroot", "virtio-blk-device,drive=systemos",
                                   "virtio-tablet-device", "virtio-keyboard-device"])
        self.assertEqual(command[command.index("-m") + 1], "6144")
        self.assertEqual(command[command.index("-cpu") + 1], "cortex-a710")
        self.assertEqual(command[command.index("-accel") + 1], "tcg,tb-size=128")
        self.assertEqual(command[command.index("-display") + 1], "gtk,zoom-to-fit=on")
        self.assertNotIn("host_device", " ".join(command))
        self.assertIn("-full-screen", command)
        self.assertEqual(command[command.index("-nic") + 1], "none")

    def test_mutated_disk_is_rejected(self):
        (self.root / "disks/os.raw").write_bytes(b"wrong")
        with self.assertRaisesRegex(ioswitch.Error, "changed"):
            ioswitch.load_bundle(self.root)

    def test_extra_esp_file_cannot_bypass_integrity_checks(self):
        (self.root / "esp/untracked.efi").write_bytes(b"extra")
        with self.assertRaisesRegex(ioswitch.Error, "Unhashed"):
            ioswitch.load_bundle(self.root)

    def test_manifest_cannot_read_outside_bundle(self):
        with self.assertRaisesRegex(ioswitch.Error, "Invalid"):
            ioswitch.contained(self.root, "../outside")
        (self.root / "escape").symlink_to(self.root.parent)
        with self.assertRaisesRegex(ioswitch.Error, "escapes"):
            ioswitch.contained(self.root, "escape/file")

    def test_internal_artifact_symlink_is_rejected(self):
        firmware = self.root / "firmware/edk2-aarch64-code.fd"
        actual = self.root / "original-code.fd"
        firmware.rename(actual)
        firmware.symlink_to(actual)
        with self.assertRaisesRegex(ioswitch.Error, "Symlinks"):
            ioswitch.load_bundle(self.root)

    def test_esp_root_symlink_is_rejected(self):
        actual = self.root / "actual-esp"
        (self.root / "esp").rename(actual)
        (self.root / "esp").symlink_to(actual, target_is_directory=True)
        with self.assertRaisesRegex(ioswitch.Error, "Symlinks"):
            ioswitch.load_bundle(self.root)

    def test_unhashed_special_esp_file_is_rejected(self):
        os.mkfifo(self.root / "esp/untracked-pipe")
        with self.assertRaisesRegex(ioswitch.Error, "Special files"):
            ioswitch.load_bundle(self.root)

    def test_manifest_cannot_claim_another_cpu_count(self):
        path = self.root / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["smp"] = 2
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ioswitch.Error, "Unsupported smp"):
            ioswitch.load_bundle(self.root)

    def test_dry_run_has_no_writes(self):
        before = set(self.root.rglob("*"))
        output = self.root.parent / "runs"
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            status = ioswitch.main(["run", "--bundle", str(self.root), "--runs", str(output), "--dry-run"])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(stdout.getvalue())[0], "qemu-system-aarch64")
        self.assertFalse(output.exists())
        self.assertEqual(before, set(self.root.rglob("*")))

    def test_cache_size_reaches_dry_run_arguments_at_both_bounds(self):
        for size in (16, 256, 1024):
            with self.subTest(size=size), contextlib.redirect_stdout(io.StringIO()) as stdout:
                status = ioswitch.main(["run", "--bundle", str(self.root), "--dry-run",
                                        "--tcg-cache-mib", str(size)])
            self.assertEqual(status, 0)
            command = json.loads(stdout.getvalue())
            self.assertEqual(command[command.index("-accel") + 1], f"tcg,tb-size={size}")

    def test_cache_size_reaches_live_run_arguments(self):
        with patch.object(ioswitch, "execute") as execute, \
                patch.object(ioswitch, "memory_available", return_value=None), \
                contextlib.redirect_stdout(io.StringIO()):
            status = ioswitch.main(["run", "--bundle", str(self.root),
                                    "--runs", str(self.root.parent / "runs"),
                                    "--display", "none", "--tcg-cache-mib", "256"])
        self.assertEqual(status, 0)
        command = execute.call_args.args[0]
        self.assertEqual(command[command.index("-accel") + 1], "tcg,tb-size=256")
        self.assertEqual(command[command.index("-display") + 1], "none")

    def test_invalid_cache_size_is_rejected_before_reading_bundle(self):
        with patch.object(ioswitch, "load_bundle") as load, contextlib.redirect_stderr(io.StringIO()):
            for value in ("15", "1025", "0", "-1", "128.5", "not-a-number"):
                with self.subTest(value=value), self.assertRaises(SystemExit) as error:
                    ioswitch.main(["run", "--bundle", str(self.root), "--tcg-cache-mib", value])
                self.assertEqual(error.exception.code, 2)
            load.assert_not_called()

    def test_smoke_retains_default_cache_and_has_no_cache_flag(self):
        command = ioswitch.qemu_base(Path("firmware.fd"), Path("run"), 4096, "cortex-a76", "none")
        self.assertEqual(command[command.index("-accel") + 1], "tcg,tb-size=128")
        with patch.object(ioswitch, "smoke") as smoke, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                ioswitch.main(["smoke", "--tcg-cache-mib", "256"])
            self.assertEqual(error.exception.code, 2)
            smoke.assert_not_called()

    def test_run_cannot_mutate_bundle(self):
        args = argparse.Namespace(bundle=self.root, runs=self.root / "esp/run", dry_run=True)
        with self.assertRaisesRegex(ioswitch.Error, "outside"):
            ioswitch.run_ipad(args)
        self.assertFalse((self.root / "esp/run").exists())

    def test_low_memory_refusal_happens_before_output_creation(self):
        output = self.root.parent / "runs"
        args = argparse.Namespace(bundle=self.root, runs=output, dry_run=False, allow_low_memory=False)
        with patch.object(ioswitch, "memory_available", return_value=1500):
            with self.assertRaisesRegex(ioswitch.Error, "Only 1500"):
                ioswitch.run_ipad(args)
        self.assertFalse(output.exists())


class PayloadTests(unittest.TestCase):
    def test_payload_matches_upstream_wire_format(self):
        result = ioswitch.payload([("kernel", b"abc"), ("cmdline", b"cpus=1\0")])
        magic, length = struct.unpack_from("<II", result)
        self.assertEqual(magic, 0x50554E58)
        self.assertEqual(length, len(result) - 8)
        name, size, reserved = struct.unpack_from("<16sII", result, 8)
        self.assertEqual((name.rstrip(b"\0"), size, reserved), (b"kernel", 3, 0))
        self.assertEqual(result[32:48], b"abc" + bytes(13))
        self.assertEqual(result[-24:], bytes(24))

    def test_kernel_log_is_not_userspace_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(ioswitch.Error, "marker not observed"):
                    ioswitch.execute([sys.executable, "-c", "print('Darwin Kernel Version 22.5.0')"],
                                     run, 3, smoke=True)
            result = json.loads((run / "result.json").read_text())
            self.assertFalse(result["xnu_userspace_confirmed"])
            self.assertFalse(result["ipad_ui_verified"])

    def test_nonzero_emulator_exit_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ioswitch.Error, "QEMU exited"):
                    ioswitch.execute([sys.executable, "-c", "raise SystemExit(7)"], Path(temporary), 3)

    def test_observation_failure_is_recorded_without_a_boot_verdict(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            with contextlib.redirect_stdout(io.StringIO()), \
                    patch.object(ioswitch, "memory_sample", return_value=None), \
                    patch.object(ioswitch, "QMP", side_effect=ValueError("Malformed QMP reply")):
                ioswitch.execute([sys.executable, "-c", "import time; time.sleep(5)"],
                                 run, 0.15, capture_every=0.05)
            result = json.loads((run / "result.json").read_text())
            self.assertEqual(result["stop_reason"], "timeout")
            self.assertTrue(result["observation_errors"])
            self.assertFalse(result["ipad_ui_verified"])
            self.assertEqual(result["frames"], [])


if __name__ == "__main__":
    unittest.main()
