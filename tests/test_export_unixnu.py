"""Synthetic fixtures test packaging, never pretend to be a booted iPadOS image."""

import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "export_unixnu", Path(__file__).resolve().parents[1] / "scripts/export_unixnu.py")
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


def adt_node(name, properties=None, children=()):
    properties = {"name": name.encode() + b"\0", **(properties or {})}
    result = struct.pack("<II", len(properties), len(children))
    for key, value in properties.items():
        result += struct.pack("<32sI", key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return result + b"".join(children)


def ipad_adt(*, model=b"iPad7,1\0", timer=62500000, ram=6144 << 20):
    return adt_node("device-tree", {"model": model}, [
        adt_node("product", {"artwork-device-idiom": b"pad\0", "product-name": b"Synthetic test iPad\0"}),
        adt_node("chosen", {"dram-base": struct.pack("<Q", 0x40000000),
                            "dram-size": struct.pack("<Q", ram),
                            "darwinboot-memory-size": struct.pack("<Q", 4096 << 20)}),
        adt_node("cpus", children=[adt_node("cpu0", {"compatible": b"arm,cortex-a710\0",
                                                   "timebase-frequency": struct.pack("<I", timer)})]),
    ])


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.esp = self.base / "prepared-run/esp"
        self.payloads = self.esp / "EFI/UniXNU/Payloads"
        self.payloads.mkdir(parents=True)
        (self.esp / "EFI/BOOT").mkdir()
        (self.esp / "EFI/BOOT/BOOTAA64.EFI").write_bytes(b"synthetic EFI fixture")
        for name in ("DarwinBoot.efi", "QemuRamfb.efi", "kernel.macho", "kernel.macho.trustcache", "scratch-data.img"):
            (self.payloads / name).write_bytes(b"synthetic fixture")
        (self.payloads / "kernel.macho.adt").write_bytes(ipad_adt())
        self.config = self.esp / "EFI/UniXNU/unixnu.toml"
        self.config.write_text('''version = 1
[[driver]]
path = "Payloads/QemuRamfb.efi"
[[driver]]
path = "Payloads/DarwinBoot.efi"
[[entry]]
id = "xnu"
type = "xnu"
path = "Payloads/kernel.macho"
initrd = "Payloads/scratch-data.img"
args = "offline-legacy-cpu-present=1"
''')
        self.firmware = self.base / "code.fd"
        self.firmware.write_bytes(b"synthetic firmware")
        self.os_disk = self.base / "OS.raw"
        raw = bytearray(4096)
        raw[512:520] = b"EFI PART"
        self.os_disk.write_bytes(raw)
        self.systemos = self.base / "SystemOS.raw"
        raw = bytearray(4096)
        raw[32:36] = b"NXSB"
        struct.pack_into("<IQ", raw, 36, 4096, 1)
        self.systemos.write_bytes(raw)
        self.output = self.base / "bundle"

    def export(self, **changes):
        options = dict(esp=self.esp, firmware=self.firmware, os_disk=self.os_disk,
                       systemos_disk=self.systemos, output=self.output, source_revision="a" * 40)
        return exporter.export_bundle(**{**options, **changes})

    def test_export_records_actual_identity_hashes_and_runtime_contract(self):
        manifest = self.export()
        self.assertEqual(manifest, json.loads((self.output / "manifest.json").read_text()))
        self.assertEqual(manifest["identity"]["model"], "iPad7,1")
        self.assertEqual(manifest["identity"]["product_name"], "Synthetic test iPad")
        self.assertEqual(manifest["ram_mib"], 6144)
        self.assertEqual(manifest["screen"], "1376x1024")
        self.assertEqual(manifest["validation"]["hardware_boot"], "not tested")
        for name, record in manifest["files"].items():
            self.assertEqual(record, exporter.file_record(self.output / name))
        self.assertEqual((self.output / manifest["os_disk"]).read_bytes(), self.os_disk.read_bytes())

    def test_rejects_wrong_identity_before_writing(self):
        (self.payloads / "kernel.macho.adt").write_bytes(ipad_adt(model=b"iPhone10,3\0"))
        with self.assertRaisesRegex(exporter.ExportError, "iPad7,1"):
            self.export()
        self.assertFalse(self.output.exists())

    def test_rejects_hvf_timer(self):
        (self.payloads / "kernel.macho.adt").write_bytes(ipad_adt(timer=24000000))
        with self.assertRaisesRegex(exporter.ExportError, "TCG requires"):
            self.export()

    def test_rejects_reduced_memory_without_a_port(self):
        (self.payloads / "kernel.macho.adt").write_bytes(ipad_adt(ram=3072 << 20))
        with self.assertRaisesRegex(exporter.ExportError, "6144"):
            self.export()

    def test_rejects_dmg_in_place_of_raw_disk(self):
        with self.os_disk.open("r+b") as stream:
            stream.seek(-512, 2)
            stream.write(b"koly")
        with self.assertRaisesRegex(exporter.ExportError, "UDIF"):
            self.export()

    def test_rejects_wrong_systemos_geometry(self):
        with self.systemos.open("r+b") as stream:
            stream.seek(40)
            stream.write(struct.pack("<Q", 2))
        with self.assertRaisesRegex(exporter.ExportError, "geometry"):
            self.export()

    def test_rejects_config_path_escape(self):
        self.config.write_text(self.config.read_text().replace("Payloads/scratch-data.img", "../../../outside"))
        with self.assertRaisesRegex(exporter.ExportError, "escapes"):
            self.export()

    def test_rejects_symlink_in_esp(self):
        (self.esp / "unsafe-link").symlink_to(self.os_disk)
        with self.assertRaisesRegex(exporter.ExportError, "symlink"):
            self.export()

    def test_preserves_existing_destination(self):
        self.output.mkdir()
        original = self.output / "user-file"
        original.write_text("keep")
        with self.assertRaisesRegex(exporter.ExportError, "already exists"):
            self.export()
        self.assertEqual(original.read_text(), "keep")

    def test_rejects_recursive_export_destination(self):
        with self.assertRaisesRegex(exporter.ExportError, "inside"):
            self.export(output=self.esp / "output")

    def test_rejects_mac_metal_payload(self):
        (self.payloads / "kernel.macho.gpuaddon").write_bytes(b"test addon")
        with self.assertRaisesRegex(exporter.ExportError, "Mac Metal"):
            self.export()

    def test_rejects_truncated_and_trailing_adt_bytes(self):
        for blob in (ipad_adt()[:-1], ipad_adt() + b"extra"):
            with self.subTest(length=len(blob)), self.assertRaises(exporter.ExportError):
                exporter.parse_adt(blob)

    def test_exported_screen_must_be_accepted_by_runner(self):
        for screen in ("112x100", "304x720", "1280x319", "8192x720", "1280x8192"):
            with self.subTest(screen=screen), self.assertRaisesRegex(exporter.ExportError, "320..4096"):
                self.export(screen=screen)
            self.assertFalse(self.output.exists())

    def test_malformed_entry_table_is_a_typed_export_error(self):
        self.config.write_text('entry = {type = "xnu", path = "Payloads/kernel.macho"}\n')
        with self.assertRaisesRegex(exporter.ExportError, "arrays of tables"):
            self.export()
        self.assertFalse(self.output.exists())

    def test_staged_bytes_are_revalidated_before_claiming_structural_success(self):
        copytree = exporter.shutil.copytree

        def changed_copy(source, destination, *args, **kwargs):
            result = copytree(source, destination, *args, **kwargs)
            if Path(source) == self.esp:
                (destination / "EFI/UniXNU/Payloads/kernel.macho.adt").write_bytes(ipad_adt(model=b"iPhone10,3\0"))
            return result

        with patch.object(exporter.shutil, "copytree", changed_copy):
            with self.assertRaisesRegex(exporter.ExportError, "iPad7,1"):
                self.export()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.base.glob(".ioswitch-export-*")), [])


if __name__ == "__main__":
    unittest.main()
