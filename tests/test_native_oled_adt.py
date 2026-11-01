"""Native board resource and Apple DT consumer contracts, without device access."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from scripts import native_oled_adt as oled
from scripts import native_xnu_layout as layout
from tests.test_native_xnu_layout import macho, trustcache


def properties(data):
    # Use the separate loader's strict reader, not the generator's serializer.
    parsed = layout.parse_adt(data)
    return {key: data[start:start + length] for key, (start, length) in parsed.properties.items()}


class NativeOledAdtTests(unittest.TestCase):
    def setUp(self):
        self.seed = bytes(range(64))
        self.data = oled.generate_adt(self.seed)
        self.props = properties(self.data)

    def test_board_resources_match_observations_and_armcpu_translation(self):
        observed = json.loads((oled.ROOT / "out/switch/native-platform-summary.json").read_text())
        self.assertEqual(self.props["/", "model"].decode().rstrip("\0"), observed["model"])
        self.assertEqual(self.props["/", "compatible"].decode().rstrip("\0").split("\0"), observed["compatible"])
        self.assertEqual(self.props["/cpus/cpu0", "compatible"], b"arm,cortex-a57\0")
        self.assertEqual(self.props["/cpus/cpu0", "device_type"], b"cpu\0")
        self.assertEqual(self.props["/cpus/cpu0", "state"], b"running\0")
        self.assertEqual(struct.unpack("<I", self.props["/cpus/cpu0", "reg"]), (0,))
        self.assertEqual(struct.unpack("<I", self.props["/cpus/cpu0", "timebase-frequency"]), (19200000,))
        self.assertEqual(self.props["/arm-io/intc", "compatible"], b"arm,cortex-a15-gic\0")
        bus, base, extent = struct.unpack("<3Q", self.props["/arm-io", "ranges"])
        reg = struct.unpack("<4Q", self.props["/arm-io/intc", "reg"])
        self.assertEqual((bus, base, extent), (0, 0x50041000, 0x1100))
        for index, observed_range in enumerate(observed["gicv2_register_ranges"]):
            address, size = reg[index * 2:index * 2 + 2]
            self.assertGreaterEqual(address, bus)
            self.assertLessEqual(address - bus + size, extent)
            self.assertEqual((base + address - bus, size),
                             (int(observed_range["base"], 0), int(observed_range["size"], 0)))
        self.assertEqual(observed["architectural_timer_interrupt_cells"][2][1] + 16, 27)

    def test_storage_reference_is_exact_opt_in_without_new_hardware_nodes(self):
        base=properties(oled.generate_adt(self.seed,userspace_fixture=True))
        probe=properties(oled.generate_adt(self.seed,userspace_fixture=True,storage_probe=True))
        self.assertEqual(probe,{**base,('/chosen','ioswitch-sd-reference'):struct.pack('<QQ',0xA001F000,0x1000)})
        for flags in ({'storage_probe':True},{'userspace_fixture':True,'storage_probe':1},
                      {'userspace_fixture':True,'storage_probe':'yes'}):
            with self.assertRaises(ValueError):oled.generate_adt(self.seed,**flags)

    def test_installed_dram_is_distinct_from_managed_memory_candidate(self):
        self.assertEqual(struct.unpack("<Q", self.props["/chosen", "dram-base"]), (0x80000000,))
        self.assertEqual(struct.unpack("<Q", self.props["/chosen", "dram-size"]), (0x100000000,))
        self.assertEqual(layout.parse_adt(self.data).window_size, 768 << 20)
        self.assertEqual(oled.MANAGED_BASE + oled.MANAGED_SIZE, 0xE2004000)
        self.assertEqual(self.props["/chosen/memory-map", "name"], b"memory-map\0")
        self.assertEqual({key for (path, key) in self.props if path == "/chosen/memory-map"}, {"name"})

    def test_root_profile_selects_exact_uuid_and_independent_data_seed_without_trust_claims(self):
        import plistlib
        props = properties(oled.generate_adt(self.seed,root_boot=True,storage_probe=True))
        expected = {**self.props,
            ('/chosen','osenvironment'): b'main\0',
            ('/defaults','kern.maxvnodes'): struct.pack('<I',13312),
            ('/chosen','crypto-hash-method'): b'sha2-384\0',
            ('/chosen','amfi-only-platform-code'): struct.pack('<I',1),
            ('/chosen','ephemeral-storage'): struct.pack('<I',1),
            ('/chosen','root-matching'): oled.ROOT_MATCHING,
            ('/chosen','ioswitch-sd-reference'): struct.pack('<QQ',0xA001F000,0x1000),
            ('/chosen/memory-map','NativeDataSeed'): bytes(16),
            ('/chosen/memory-map','TrustCache'): bytes(16),
            ('/chosen/manifest-properties','name'): b'manifest-properties\0',
            ('/product','name'): b'product\0',
            ('/product','product-name'): oled.MODEL.encode()+b'\0',
            ('/product','model'): oled.MODEL.encode()+b'\0'}
        self.assertEqual(props,expected)
        xml = b'<?xml version="1.0"?><plist version="1.0">'+props['/chosen','root-matching'][:-1]+b'</plist>'
        self.assertEqual(plistlib.loads(xml),{'IOProviderClass':'IOMedia','IOPropertyMatch':{'UUID':oled.ROOT_VOLUME_UUID}})
        for key in ('RAMDisk','PackedRAMDisk','boot-uuid','boot-manifest-hash','owner-boot-manifest-domain','vmm-present'):
            self.assertNotIn(key,{k for _,k in props})
        for flags in ({'root_boot':True},{'root_boot':True,'userspace_fixture':True,'storage_probe':True},
                      {'root_boot':1,'storage_probe':True},{'root_boot':'yes','storage_probe':True}):
            with self.subTest(flags=flags),self.assertRaises(ValueError):oled.generate_adt(self.seed,**flags)

    def test_root_vnode_budget_follows_managed_ram_without_changing_fixture_profiles(self):
        props = properties(oled.generate_adt(self.seed, root_boot=True, storage_probe=True))
        managed = struct.unpack("<Q", props["/chosen", "darwinboot-memory-size"])[0]
        self.assertEqual(managed, 768 << 20)
        self.assertEqual(struct.unpack("<I", props["/defaults", "kern.maxvnodes"])[0],
                         managed // 65536 + 1024)
        for flags in ({}, {"userspace_fixture": True},
                      {"userspace_fixture": True, "storage_probe": True}):
            self.assertNotIn(("/defaults", "kern.maxvnodes"),
                             properties(oled.generate_adt(self.seed, **flags)))

    def test_exact_minimal_nodes_exclude_model_storage_and_security_claims(self):
        self.assertEqual({path for path, key in self.props}, {
            "/", "/chosen", "/chosen/memory-map", "/cpus", "/cpus/cpu0",
            "/defaults", "/options", "/arm-io", "/arm-io/intc"})
        for key in ("vmm-present", "RAMDisk", "TrustCache", "root-matching", "boot-manifest-hash",
                    "crypto-hash-method", "osenvironment", "clock-frequency", "bus-frequency",
                    "cpu-impl-reg", "interrupt-controller"):
            self.assertNotIn(key, {key for path, key in self.props})
        for marker in (b"qemu", b"dummy-virt", b"iPad", b"virtio", b"pl011", b"ufshc"):
            self.assertNotIn(marker, self.data)

    def test_volatile_chrp_bank_checksums_partition_bounds_and_empty_variables(self):
        image = self.props["/chosen", "nvram-proxy-data"]
        self.assertEqual(len(image), 8192)
        self.assertEqual(struct.unpack("<I", self.props["/chosen", "nvram-bank-size"]), (len(image),))
        self.assertEqual(struct.unpack("<I", self.props["/chosen", "nvram-bank-count"]), (1,))
        self.assertEqual(struct.unpack("<I", self.props["/chosen", "nvram-current-bank"]), (0,))
        self.assertEqual(struct.unpack_from("<I", image, 16)[0], zlib.adler32(image[20:]))
        cursor, found = 0, []
        while cursor < len(image):
            signature, checksum, blocks, name = struct.unpack_from("<BBH12s", image, cursor)
            self.assertEqual(signature, 0x70)
            total = signature + sum(image[cursor + 2:cursor + 16])
            while total > 255:
                total = (total & 255) + (total >> 8)
            self.assertEqual(checksum, total)
            size = blocks * 16
            self.assertGreaterEqual(size, 16)
            self.assertLessEqual(cursor + size, len(image))
            found.append((name.rstrip(b"\0"), cursor, size))
            if name.startswith(b"common"):
                self.assertEqual(image[cursor + 16:cursor + size], bytes(2048))
            cursor += size
        self.assertEqual(found, [(b"nvram", 0, 32), (b"common", 32, 2064), (b"free", 2096, 6096)])
        self.assertEqual(image[20:32], bytes(12))

    def test_seed_is_exact_and_default_uses_host_entropy_once(self):
        self.assertEqual(self.props["/chosen", "random-seed"], self.seed)
        with patch.object(oled.os, "urandom", return_value=b"r" * 64) as random:
            data = oled.generate_adt()
            random.assert_called_once_with(64)
        self.assertEqual(properties(data)["/chosen", "random-seed"], b"r" * 64)
        for bad in (b"", b"r" * 63, b"r" * 65, "r" * 64, bytearray(64)):
            with self.subTest(value=type(bad)), self.assertRaises(ValueError):
                oled.generate_adt(bad)

    def test_userspace_fixture_requires_real_sidecars_and_preserves_board_resources(self):
        data = oled.generate_adt(self.seed, userspace_fixture=True)
        props = properties(data)
        added = {
            ("/chosen", "osenvironment"): b"ramdisk\0",
            ("/chosen", "crypto-hash-method"): b"sha2-384\0",
            ("/chosen", "amfi-only-platform-code"): struct.pack("<I", 1),
            ("/chosen/memory-map", "RAMDisk"): bytes(16),
            ("/chosen/memory-map", "TrustCache"): bytes(16),
            ("/chosen/manifest-properties", "name"): b"manifest-properties\0",
            ("/product", "name"): b"product\0",
            ("/product", "product-name"): oled.MODEL.encode()+b"\0",
            ("/product", "model"): oled.MODEL.encode()+b"\0",
        }
        self.assertEqual(props, {**self.props, **added})
        for invalid in (1, None, "yes"):
            with self.assertRaises(ValueError):
                oled.generate_adt(self.seed, userspace_fixture=invalid)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            kernel, tree, disk, cache = [root / n for n in ("kernel", "adt", "disk", "cache")]
            kernel.write_bytes(macho(base=0xFFFFFFF007004000))
            tree.write_bytes(data)
            disk.write_bytes(b"owned userspace fixture\0" * 300)
            cache.write_bytes(trustcache())
            kwargs = dict(physical_base=oled.MANAGED_BASE, window_size=oled.MANAGED_SIZE,
                          reserved_ranges=[])
            for absent in ({}, {"ramdisk_path": disk}, {"trustcache_path": cache}):
                with self.assertRaises(layout.LayoutError):
                    layout.build_layout(kernel, tree, root / "missing", **kwargs, **absent)
                self.assertFalse((root / "missing").exists())
            result = layout.build_layout(kernel, tree, root / "complete", **kwargs,
                                         ramdisk_path=disk, trustcache_path=cache)
            payload = (root / "complete/kernel-window.bin").read_bytes()
            offset = result["adt_pa"] - result["phys_base"]
            patched = payload[offset:offset + result["adt_size"]]
            original_fields = layout.parse_adt(data).properties
            ranges = {r["name"]: r for r in result["ranges"]}
            for key, name, source in (("RAMDisk", "ramdisk", disk), ("TrustCache", "trustcache", cache)):
                region = ranges[name]
                field_offset, field_size = original_fields["/chosen/memory-map", key]
                self.assertEqual(field_size, 16)
                self.assertEqual(struct.unpack_from("<QQ", patched, field_offset),
                                 (region["start"], region["size"]))
                begin = region["image_offset"]
                self.assertEqual(payload[begin:begin + source.stat().st_size], source.read_bytes())
                self.assertLessEqual(region["end"], result["top_of_kernel_data"])
            self.assertLess(result["used_extent"], 0x6000000)
            self.assertEqual(tree.read_bytes(), data)

    def test_local_build_records_host_provenance_and_rejects_overwrite(self):
        with tempfile.TemporaryDirectory(dir=oled.ROOT / "out", prefix="adt-test-") as temp:
            output = Path(temp) / "tree"
            with patch.object(oled.os, "urandom", return_value=b"h" * 64):
                result = oled.build(output)
            data = (output / "kernel.adt").read_bytes()
            self.assertEqual(result["files"]["kernel.adt"], {
                "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
            self.assertEqual(json.loads((output / "manifest.json").read_text()), result)
            self.assertFalse(result["random_seed"]["hardware_rng"])
            self.assertFalse(result["random_seed"]["fresh_per_boot"])
            self.assertFalse(result["native_window_ownership_verified"])
            self.assertFalse(result["native_xnu_boot_verified"])
            self.assertFalse(result["nvram"]["persistent"])
            self.assertEqual(len(result["evidence"]), 5)
            self.assertEqual(len(result["audited_consumers"]), 10)
            with self.assertRaises(ValueError):
                oled.build(output)
            self.assertEqual((output / "kernel.adt").read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
