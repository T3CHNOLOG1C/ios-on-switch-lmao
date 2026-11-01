import ast
import errno
import gzip
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/collect_switch.py"
SPEC = importlib.util.spec_from_file_location("collect_switch", SCRIPT)
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


VULKAN = """Vulkan Instance Version: 1.3.239
DISPLAY=hostname-private:0
GPU0:
    apiVersion = 1.2.170 (4202666)
    driverVersion = 32.7.6
    vendorID = 0x10de
    deviceID = 0x1234
    deviceName = NVIDIA Tegra X1
    deviceType = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    driverName = NVIDIA
    driverInfo = /home/private-user/build
    deviceUUID = 12345678-1234-1234-1234-123456789abc
    pipelineCacheUUID = do-not-report-this
Device Extensions: count = 2
    VK_EXT_external_memory_host : extension revision 1
    VK_KHR_external_memory_fd = 1
    minImportedHostPointerAlignment = 4096
    timelineSemaphore = true
    bufferDeviceAddress = false
    storageBuffer16BitAccess = true
        MEMORY_PROPERTY_HOST_VISIBLE_BIT
        MEMORY_PROPERTY_HOST_COHERENT_BIT
GPU1:
    apiVersion = 1.3.239
    deviceName = llvmpipe (LLVM 15.0.6, 256 bits)
    deviceType = PHYSICAL_DEVICE_TYPE_CPU
    timelineSemaphore = false
    driverUUID = do-not-report-this-either
"""


def fake_runner(name, arguments):
    if name == "vulkaninfo":
        output = VULKAN
    elif name == "glxinfo":
        output = """name of display: hostname-private:0
OpenGL vendor string: NVIDIA Corporation
OpenGL renderer string: NVIDIA Tegra X1
OpenGL version string: 4.6.0 NVIDIA 32.7.6
OpenGL driver private path: /home/private-user/driver
"""
    else:
        output = {
            ("--version",): "QEMU emulator version 8.2.0 (/home/private-user/custom-build)\nCopyright...",
            ("-accel", "help"): "Accelerators supported in QEMU binary:\ntcg\nkvm\n",
            ("-cpu", "help"): "Available CPUs:\n  cortex-a57\n  cortex-a710\n  max\n  host\n",
            ("-display", "help"): "Available display backend types:\ngtk\nsdl\nnone\n",
        }[tuple(arguments)]
    return {"status": "ok", "returncode": 0, "stdout": output}


class CollectorTests(unittest.TestCase):
    def write(self, root, name, value):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)

    def test_synthetic_linux_inventory_and_privacy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(root, "etc/os-release", 'ID=ubuntu\nNAME="Ubuntu Linux"\nVERSION_ID="24.04"\nHOME_URL="https://private-host/"\n')
            self.write(root, "proc/cpuinfo", "processor : 0\nFeatures : fp asimd aes sha2\nCPU implementer : 0x41\nCPU part : 0xd07\nSerial : secret-serial\nHardware : private-host\n")
            self.write(root, "proc/meminfo", "MemTotal: 4000000 kB\nMemAvailable: 1234567 kB\nSwapTotal: 2000000 kB\nSwapFree: 1234 kB\n")
            self.write(root, "proc/swaps", "Filename Type Size Used Priority\n/home/private-user/swap file 2048 100 -2\n/dev/private-label partition 4096 200 5\n")
            self.write(root, "proc/device-tree/model", "Nintendo Switch OLED\0")
            self.write(root, "proc/device-tree/compatible", "nintendo,odin\0nvidia,tegra210\0")
            self.write(root, "sys/block/zram0/disksize", "2147483648\n")
            self.write(root, "sys/block/zram0/mm_stat", "1024 512 768 0 1024 3 4\n")
            self.write(root, "sys/block/zram0/comp_algorithm", "lzo [lz4] zstd\n")
            with gzip.open(root / "proc/config.gz", "wb") as stream:
                stream.write(b"CONFIG_KVM=y\n# CONFIG_KVM_ARM_HOST is not set\nCONFIG_LOCALVERSION=secret-label\n")
            self.write(root, "dev/kvm", "not a device")
            report = collector.collect(root, True, root=root, runner=fake_runner,
                                       identity=("Linux", "aarch64", "4.9.337-switchroot"))
            encoded = json.dumps(report)
            for secret in ("secret-serial", "private-host", "hostname-private", "private-user", "private-label",
                           "secret-label", "UUID", "do-not-report", directory):
                self.assertNotIn(secret, encoded)
            self.assertEqual(report["os_release"]["data"]["NAME"], "Ubuntu Linux")
            self.assertEqual(report["cpu"]["data"][0]["part"], "0xd07")
            self.assertEqual(report["memory"]["data"]["available_bytes"], 1234567 * 1024)
            self.assertEqual(report["swap"]["data"][1]["used_bytes"], 200 * 1024)
            self.assertEqual(report["device_tree"]["compatible"]["values"], ["nintendo,odin", "nvidia,tegra210"])
            self.assertEqual(report["zram"]["devices"][0]["active_compressor"], "lz4")
            self.assertIsNone(report["zram"]["devices"][0]["mm_stat"]["huge_pages"])
            self.assertEqual(report["kernel_config"]["flags"]["CONFIG_KVM"], "y")
            self.assertEqual(report["kernel_config"]["flags"]["CONFIG_KVM_ARM_HOST"], "n")
            self.assertIsNone(report["kernel_config"]["flags"]["CONFIG_HAVE_KVM"])
            self.assertEqual(report["kvm"]["api_query_status"], "not_character_device")
            self.assertEqual(report["qemu"]["version"]["value"], "8.2.0")
            self.assertTrue(report["qemu"]["cpus"]["listed"]["cortex-a710"])
            self.assertFalse(report["qemu"]["cpus"]["listed"]["cortex-a76"])
            self.assertFalse(report["boot_test_performed"])

    def test_vulkan_device_boundaries_and_unknown_features(self):
        devices = collector.parse_vulkaninfo(VULKAN)
        self.assertEqual(len(devices), 2)
        self.assertTrue(devices[0]["features"]["timelineSemaphore"])
        self.assertFalse(devices[1]["features"]["timelineSemaphore"])
        self.assertIsNone(devices[1]["features"]["storageBuffer16BitAccess"])
        self.assertTrue(devices[0]["extensions_listed"]["VK_EXT_external_memory_host"])
        self.assertTrue(devices[0]["extensions_listed"]["VK_KHR_external_memory_fd"])
        self.assertIsNone(devices[1]["extensions_listed"]["VK_EXT_external_memory_host"])
        self.assertEqual(devices[0]["minImportedHostPointerAlignment"], 4096)
        self.assertEqual(devices[0]["host_memory_flags_observed"], ["HOST_VISIBLE", "HOST_COHERENT"])
        self.assertIsNone(devices[0]["properties"]["driverInfo"])

    def test_missing_sources_do_not_become_unsupported(self):
        unavailable = lambda *_: {"status": "unavailable", "returncode": None, "stdout": ""}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = collector.collect(root, root=root, runner=unavailable, identity=("Linux", "aarch64", "unknown"))
            self.assertIsNone(report["memory"]["data"])
            self.assertIsNone(report["zram"]["devices"])
            self.assertEqual(report["kernel_config"]["status"], "unavailable")
            self.assertIsNone(report["qemu"]["accelerators"]["listed"]["kvm"])
            self.assertFalse(report["kvm"]["exists"])
            self.assertIsNone(report["kvm"]["api_version"])
            self.assertEqual(report["graphics"]["status"], "not_requested")
            self.assertEqual(list(root.iterdir()), [])

    def test_config_fallback_and_no_inferred_false(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(root, "proc/config.gz", "invalid gzip")
            self.write(root, "boot/config-4.9-test", "CONFIG_KVM=m\n# CONFIG_ARM64_VHE is not set\n")
            report = collector.collect_config(root, "4.9-test")
            self.assertEqual(report["source"], "boot_config")
            self.assertEqual(report["flags"]["CONFIG_KVM"], "m")
            self.assertEqual(report["flags"]["CONFIG_ARM64_VHE"], "n")
            self.assertIsNone(report["flags"]["CONFIG_KVM_ARM_HOST"])

    def test_kvm_query_uses_read_only_open_and_only_api_ioctl(self):
        with patch.object(Path, "stat", return_value=SimpleNamespace(st_mode=stat.S_IFCHR | 0o660)), \
                patch.object(collector.os, "access", return_value=True), \
                patch.object(collector.os, "open", return_value=123) as opened, \
                patch("fcntl.ioctl", return_value=12) as ioctl, \
                patch.object(collector.os, "close") as closed:
            report = collector.collect_kvm(Path("/"))
        self.assertEqual(opened.call_args.args[1] & os.O_ACCMODE, os.O_RDONLY)
        ioctl.assert_called_once_with(123, 0xAE00)
        closed.assert_called_once_with(123)
        self.assertEqual(report["api_version"], 12)
        self.assertFalse(report["vm_created"])

    def test_kvm_denied_is_not_absent(self):
        with patch.object(Path, "stat", return_value=SimpleNamespace(st_mode=stat.S_IFCHR | 0o660)), \
                patch.object(collector.os, "access", return_value=False), \
                patch.object(collector.os, "open", side_effect=PermissionError(errno.EACCES, "/private-secret")), \
                patch("fcntl.ioctl") as ioctl:
            report = collector.collect_kvm(Path("/"))
        ioctl.assert_not_called()
        self.assertTrue(report["exists"])
        self.assertIsNone(report["api_version"])
        self.assertEqual(report["api_query_status"], "EACCES")
        self.assertNotIn("private-secret", json.dumps(report))

    def test_storage_does_not_create_missing_path(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "not-created"
            report = collector.storage_report(missing)
            self.assertIsNone(report["available_bytes"])
            self.assertFalse(missing.exists())
            self.assertNotIn(directory, json.dumps(report))

    def test_output_is_exclusive_including_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            collector.write_report({"known": True}, output)
            expected = output.read_bytes()
            self.assertEqual(json.loads(expected), {"known": True})
            with self.assertRaises(FileExistsError):
                collector.write_report({"replacement": True}, output)
            self.assertEqual(output.read_bytes(), expected)
            link = Path(directory) / "link.json"
            link.symlink_to(Path(directory) / "missing")
            with self.assertRaises(FileExistsError):
                collector.write_report({}, link)
            self.assertFalse((Path(directory) / "missing").exists())

    def test_bounded_command_timeout_and_output_limit_discard_partial_data(self):
        result = collector.run_command(sys.executable, ["-c", "import time; print('partial'); time.sleep(5)"], timeout=0.1)
        self.assertEqual(result["status"], "timeout")
        self.assertEqual(result["stdout"], "")
        result = collector.run_command(sys.executable, ["-c", "print('x' * 10000)"], limit=100)
        self.assertEqual(result["status"], "output_limit")
        self.assertEqual(result["stdout"], "")

    def test_failed_or_unrecognized_command_does_not_claim_features(self):
        result = collector.run_command(sys.executable, ["-c", "import sys; print('secret diagnostic'); sys.exit(2)"])
        self.assertEqual(result["status"], "command_failed")
        self.assertEqual(result["stdout"], "")
        self.assertEqual(collector.parse_qemu_help("unknown output", "Available CPUs", ["host"]), {"host": None})
        self.assertEqual(collector.parse_vulkaninfo("GPU0:\nunknownVersion = private\n"), [])
        self.assertIsNone(collector.parse_swaps("unrecognized output"))
        self.assertEqual(collector.parse_swaps("Filename Type Size Used Priority\n"), [])

    def test_python38_syntax(self):
        ast.parse(SCRIPT.read_text(), feature_version=(3, 8))


if __name__ == "__main__":
    unittest.main()
