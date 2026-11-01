import copy
from contextlib import ExitStack
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/export_os_disk.py"
SPEC = importlib.util.spec_from_file_location("export_os_disk", SCRIPT)
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


class ExportOSDiskTests(unittest.TestCase):
    def setUp(self):
        self.source = Path("/tmp/ioswitch-test-image.dmg").resolve()
        self.attachment = {"image-path": str(self.source), "writeable": False,
                           "system-entities": [{"dev-entry": "/dev/disk22", "content-hint": "GUID_partition_scheme"}]}
        self.images = {"images": [self.attachment]}
        self.info = {"BusProtocol": "Disk Image", "Writable": False,
                     "DeviceNode": "/dev/disk22", "TotalSize": exporter.OS_BYTES}

    def test_derives_raw_device_only_for_validated_mapping(self):
        self.assertEqual(exporter.validate_mapping(self.images, self.source, "/dev/disk22", self.info), "/dev/rdisk22")

    def test_rejects_real_disk_writable_disk_wrong_size_and_wrong_identity(self):
        for key, value in (("BusProtocol", "PCI-Express"), ("Writable", True),
                           ("TotalSize", exporter.OS_BYTES - 1), ("DeviceNode", "/dev/disk0")):
            with self.subTest(key=key):
                info = dict(self.info, **{key: value})
                with self.assertRaises(exporter.ExportError):
                    exporter.validate_mapping(self.images, self.source, "/dev/disk22", info)

    def test_rejects_missing_duplicate_and_writable_source_attachments(self):
        for images in ({"images": []}, {"images": [self.attachment, self.attachment]},
                       {"images": [dict(self.attachment, writeable=True)]}):
            with self.assertRaises(exporter.ExportError):
                exporter.validate_mapping(images, self.source, "/dev/disk22", self.info)

    def test_rejects_partition_arbitrary_path_and_ambiguous_whole_disk(self):
        for name in ("/dev/disk22s1", "/dev/rdisk22", "/tmp/disk22", "disk22", "/dev/disk22/../disk0"):
            attachment = copy.deepcopy(self.attachment)
            attachment["system-entities"][0]["dev-entry"] = name
            with self.assertRaises(exporter.ExportError):
                exporter.whole_disk(attachment)
        attachment = copy.deepcopy(self.attachment)
        attachment["system-entities"] *= 2
        with self.assertRaises(exporter.ExportError):
            exporter.whole_disk(attachment)

    def test_copy_checks_exact_bytes_size_and_hash(self):
        data = b"authenticated disk bytes"
        sha = hashlib.sha256(data).hexdigest()
        output = io.BytesIO()
        self.assertEqual(exporter.copy_verified(io.BytesIO(data), output, len(data), sha), sha)
        self.assertEqual(output.getvalue(), data)
        for source, size, expected in ((data[:-1], len(data), sha), (data + b"x", len(data), sha),
                                       (data, len(data), "0" * 64)):
            with self.assertRaises(exporter.ExportError):
                exporter.copy_verified(io.BytesIO(source), io.BytesIO(), size, expected)

    def test_rejects_malformed_or_unverified_policy(self):
        good = {"image_bytes": exporter.OS_BYTES, "volume_uuid": exporter.OS_UUID,
                "source_compressed_sha256": exporter.SOURCE_SHA256,
                "real_c_signature_and_block_verification": True,
                "original_image_modified": False, "image_sha256": "0" * 64}
        exporter.validate_policy(good)
        for policy in (None, [], dict(good, image_sha256=None), dict(good, image_bytes=1),
                       dict(good, original_image_modified=True),
                       dict(good, real_c_signature_and_block_verification=False)):
            with self.subTest(policy=policy), self.assertRaises(exporter.ExportError):
                exporter.validate_policy(policy)

    def test_cleanup_does_not_detach_a_reused_or_changed_device(self):
        with mock.patch.object(exporter, "plist", return_value={"images": []}), \
                mock.patch.object(exporter.subprocess, "run") as detach:
            exporter.detach_owned_image(self.source, "/dev/disk22")
            detach.assert_not_called()
        changed = copy.deepcopy(self.images)
        changed["images"][0]["system-entities"][0]["dev-entry"] = "/dev/disk23"
        with mock.patch.object(exporter, "plist", return_value=changed), \
                mock.patch.object(exporter.subprocess, "run") as detach, \
                self.assertRaises(exporter.ExportError):
            exporter.detach_owned_image(self.source, "/dev/disk22")
        detach.assert_not_called()

    def test_cleanup_refuses_a_physical_device(self):
        info = dict(self.info, BusProtocol="PCI-Express")
        with mock.patch.object(exporter, "plist", side_effect=[self.images, info]), \
                mock.patch.object(exporter.subprocess, "run") as detach, \
                self.assertRaises(exporter.ExportError):
            exporter.detach_owned_image(self.source, "/dev/disk22")
        detach.assert_not_called()

    def test_ambiguous_attachment_reports_cleanup_limit_without_guessing(self):
        with mock.patch.object(exporter, "plist", return_value={"system-entities": []}), \
                mock.patch.object(exporter.subprocess, "run") as detach, \
                self.assertRaisesRegex(exporter.ExportError, "safe cleanup"):
            with exporter.attached_image(self.source):
                self.fail("ambiguous disk attachment must not be usable")
        detach.assert_not_called()


class ExportLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory())).resolve()
        self.image = self.root / "source.dmg"
        self.image.write_bytes(b"compressed source fixture")
        self.raw = self.root / "raw-fixture"
        self.data = b"authenticated virtual disk bytes"
        self.raw.write_bytes(self.data)
        self.output = self.root / "OS.raw"
        self.verification = self.root / "verification.json"
        self.policy = {"image_bytes": len(self.data), "volume_uuid": exporter.OS_UUID,
                       "source_compressed_sha256": exporter.digest(self.image),
                       "real_c_signature_and_block_verification": True,
                       "original_image_modified": False,
                       "image_sha256": hashlib.sha256(self.data).hexdigest()}
        self.verification.write_text(json.dumps(self.policy))
        self.attached = False
        self.detached = False
        self.detach_error = None
        self.competing_output = False
        self.attachment = {"image-path": str(self.image), "writeable": False,
                           "system-entities": [{"dev-entry": "/dev/disk22", "content-hint": "GUID_partition_scheme"}]}
        self.info = {"BusProtocol": "Disk Image", "Writable": False,
                     "DeviceNode": "/dev/disk22", "TotalSize": len(self.data)}
        real_open = os.open

        def open_device(path, flags, *args, **kwargs):
            if path == "/dev/rdisk22":
                self.assertEqual(flags, os.O_RDONLY | os.O_NOFOLLOW)
                return real_open(self.raw, flags)
            return real_open(path, flags, *args, **kwargs)

        def image_plist(*command):
            if command[:2] == ("hdiutil", "info"):
                return {"images": [self.attachment] if self.attached else []}
            if command[:2] == ("hdiutil", "attach"):
                self.assertIn("-readonly", command)
                self.assertEqual(command[-1], str(self.image))
                self.attached = True
                return self.attachment
            if command == ("diskutil", "info", "-plist", "/dev/disk22"):
                return self.info
            self.fail(f"unexpected plist command: {command}")

        def detach(command, **kwargs):
            self.assertEqual(command, ["hdiutil", "detach", "/dev/disk22"])
            self.assertFalse(self.output.exists())
            self.assertTrue(kwargs["capture_output"])
            if self.detach_error:
                raise self.detach_error
            self.attached = False
            self.detached = True
            if self.competing_output:
                self.output.write_bytes(b"someone else's output")
            return subprocess.CompletedProcess(command, 0, "disk22 ejected", "")

        for patch in (mock.patch.object(exporter, "OS_BYTES", len(self.data)),
                      mock.patch.object(exporter, "SOURCE_SHA256", self.policy["source_compressed_sha256"]),
                      mock.patch.object(exporter, "RESERVE", 0),
                      mock.patch.object(exporter, "plist", side_effect=image_plist),
                      mock.patch.object(exporter.os, "open", side_effect=open_device),
                      mock.patch.object(exporter.stat, "S_ISCHR", return_value=True),
                      mock.patch.object(exporter.subprocess, "run", side_effect=detach)):
            self.stack.enter_context(patch)

    def assert_no_partial(self):
        self.assertEqual(list(self.root.glob(".ioswitch-os-*.partial")), [])

    def test_publishes_after_detach_and_hashes_the_policy_bytes_used(self):
        original_policy = self.verification.read_bytes()
        real_copy = exporter.copy_verified

        def replace_policy(*args):
            self.verification.write_text('{"replaced": true}')
            return real_copy(*args)

        with mock.patch.object(exporter, "copy_verified", side_effect=replace_policy):
            result = exporter.export(self.image, self.verification, self.output)
        self.assertTrue(self.detached)
        self.assertEqual(self.output.read_bytes(), self.data)
        self.assertEqual(result["policy_sha256"], hashlib.sha256(original_policy).hexdigest())
        self.assert_no_partial()

    def test_failed_detach_does_not_publish_or_leave_partial_output(self):
        self.detach_error = subprocess.CalledProcessError(16, ["hdiutil", "detach", "/dev/disk22"])
        with self.assertRaises(subprocess.CalledProcessError):
            exporter.export(self.image, self.verification, self.output)
        self.assertFalse(self.output.exists())
        self.assert_no_partial()

    def test_hash_failure_still_detaches_and_removes_partial(self):
        self.raw.write_bytes(b"X" * len(self.data))
        with self.assertRaisesRegex(exporter.ExportError, "authenticated policy"):
            exporter.export(self.image, self.verification, self.output)
        self.assertTrue(self.detached)
        self.assertFalse(self.output.exists())
        self.assert_no_partial()

    def test_reports_copy_and_cleanup_failures_together(self):
        self.raw.write_bytes(b"X" * len(self.data))
        self.detach_error = subprocess.CalledProcessError(16, ["hdiutil", "detach", "/dev/disk22"])
        with self.assertRaisesRegex(exporter.ExportError, "authenticated policy.*cleanup also failed"):
            exporter.export(self.image, self.verification, self.output)
        self.assertFalse(self.output.exists())
        self.assert_no_partial()

    def test_destination_created_during_copy_is_preserved(self):
        self.competing_output = True
        with self.assertRaises(FileExistsError):
            exporter.export(self.image, self.verification, self.output)
        self.assertTrue(self.detached)
        self.assertEqual(self.output.read_bytes(), b"someone else's output")
        self.assert_no_partial()


if __name__ == "__main__":
    unittest.main()
