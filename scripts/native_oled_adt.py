#!/usr/bin/env python3
"""Build the Apple DeviceTree for the native OLED trial and opt-in SMP.

This is a fixed board description, not a conversion of the QEMU tree. It does
not establish memory ownership, interrupt delivery, or a successful XNU boot.
Only build() draws a production seed; the optional generate_adt seed is for
host tests. No SD card, device, firmware variable or NVRAM controller is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import zlib

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts.package_native_handoff import ROOT, read_regular, safe_output

MANAGED_BASE = 0xB2004000
MANAGED_USER_BASE = 0xB207C000
MANAGED_SIZE = 768 << 20
DRAM_BASE = 0x80000000
DRAM_SIZE = 4 << 30
TIMEBASE_HZ = 19_200_000
GICD_BASE, GICD_SIZE = 0x50041000, 0x1000
GICC_BASE, GICC_SIZE = 0x50042000, 0x100
MODEL = "Nintendo Switch (OLED model)"
COMPATIBLE = ("nvidia,fric", "nintendo,aula", "nintendo,nx", "nvidia,tegra210b01")
ROOT_VOLUME_UUID = "04769D26-2CA0-4825-B443-E2E5F88300F2"
ROOT_MATCHING = ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                 '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                 ROOT_VOLUME_UUID + '</string></dict></dict>').encode("ascii") + b"\0"

EVIDENCE = (
    "out/switch/native-platform-summary.json",
    "out/switch/native-live-hardware.json",
    "out/switch/native-probe-v3-hardware-result.json",
    "out/switch/native-handoff-v2-hardware-result.json",
    "out/switch/native-memory-hardware-result.json",
)
CONSUMERS = (
    "drivers/ArmCpu/ArmCpu.cpp",
    "drivers/ArmDTPlatform/ArmDTPlatform.cpp",
    "drivers/ArmDTPlatform/Info.plist",
    "refer/xnu/osfmk/arm/arm_init.c",
    "refer/xnu/osfmk/arm64/arm_vm_init.c",
    "refer/xnu/osfmk/arm64/machine_routines.c",
    "refer/xnu/pexpert/arm/pe_identify_machine.c",
    "refer/xnu/pexpert/gen/pe_gen.c",
    "refer/xnu/iokit/Kernel/IONVRAM.cpp",
    "refer/xnu/iokit/Kernel/IONVRAMCHRPHandler.cpp",
)


def _node(name: str, properties=(), children=()) -> bytes:
    """Apple DT: LE counts, 32-byte property names, and 4-byte value padding."""
    properties = [("name", name.encode("ascii") + b"\0"), *properties]
    result = struct.pack("<II", len(properties), len(children))
    for key, value in properties:
        result += struct.pack("<32sI", key.encode("ascii"), len(value))
        result += value + bytes(-len(value) % 4)
    return result + b"".join(children)


def _empty_nvram_proxy() -> list[tuple[str, bytes]]:
    """An empty, volatile CHRP v1 bank for the actual IODTNVRAM consumer.

    ArmDTPlatform requires this bootstrap service. The 8 KiB bank and 2 KiB
    common partition are software allocations, not claimed OLED hardware.
    No system partition, policy variable, controller or persistence is supplied.
    """
    def header(name: bytes, size: int) -> bytes:
        data = bytearray(struct.pack("<BBH12s", 0x70, 0, size // 16, name))
        checksum = sum(data)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        data[1] = checksum
        return bytes(data)

    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b"free", len(image) - free_at)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))
    return [("nvram-bank-size", struct.pack("<I", len(image))),
            ("nvram-bank-count", struct.pack("<I", 1)),
            ("nvram-current-bank", struct.pack("<I", 0)),
            ("nvram-proxy-data", bytes(image))]


def generate_adt(seed: bytes | None = None, *, userspace_fixture: bool = False,
                 storage_probe: bool = False, root_boot: bool = False, full_ram: bool = False,
                 native_smp: bool = False) -> bytes:
    """Serialize only the audited native resources; deterministic seed for tests."""
    if seed is None:
        seed = os.urandom(64)
    if not isinstance(seed, bytes) or len(seed) != 64:
        raise ValueError("random seed must be exactly 64 bytes")
    if type(userspace_fixture) is not bool:
        raise ValueError("userspace_fixture must be a boolean")
    if type(root_boot) is not bool or (root_boot and userspace_fixture):
        raise ValueError("root_boot must be a boolean and excludes the userspace fixture")
    if type(storage_probe) is not bool or (storage_probe and not (userspace_fixture or root_boot)):
        raise ValueError("storage_probe requires an explicit userspace or root profile")
    if root_boot and not storage_probe:
        raise ValueError("root_boot requires the same-boot native storage reference")
    if type(full_ram) is not bool:
        raise ValueError("full_ram must be boolean")
    if full_ram and not root_boot:
        raise ValueError("full_ram requires native root profile")
    if type(native_smp) is not bool or (native_smp and not (root_boot and full_ram)):
        raise ValueError("native_smp requires the explicit full-RAM native root profile")
    # Sidecar slots stay empty until native_xnu_layout checks and places their
    # exact bytes inside the managed kernel window. The root profile uses a
    # disposable Data seed; the earlier fixture profile uses a RAM root.
    bootstrap = ([
        ("osenvironment", b"main\0" if root_boot else b"ramdisk\0"),
        ("crypto-hash-method", b"sha2-384\0"),
        ("amfi-only-platform-code", struct.pack("<I", 1)),
    ] if userspace_fixture or root_boot else [])
    if root_boot:
        bootstrap += [("ephemeral-storage", struct.pack("<I", 1)),
                      ("root-matching", ROOT_MATCHING)]
    if storage_probe:
        bootstrap.append(("ioswitch-sd-reference", struct.pack("<QQ", 0xA001F000, 0x1000)))
    if full_ram:
        bootstrap.append(("ioswitch-ram-ledger", bytes(16+32+96*16)))
    memory_map = ([("NativeDataSeed", bytes(16)), ("TrustCache", bytes(16))] if root_boot else
                  [("RAMDisk", bytes(16)), ("TrustCache", bytes(16))] if userspace_fixture else [])
    return _node("device-tree", [
        ("model", MODEL.encode("ascii") + b"\0"),
        ("compatible", b"\0".join(s.encode("ascii") for s in COMPATIBLE) + b"\0"),
    ], [
        _node("chosen", [
            ("random-seed", seed),
            # arm_init copies actual installed DRAM metadata to gDramBase/Size.
            # boot_args physBase/memSize alone define the managed allocation.
            ("dram-base", struct.pack("<Q", DRAM_BASE)),
            ("dram-size", struct.pack("<Q", DRAM_SIZE)),
            # Host layout request, not a claim that this window is owned.
            ("darwinboot-memory-size", struct.pack("<Q", (4 << 30) if full_ram else MANAGED_SIZE)),
            *_empty_nvram_proxy(),
            *bootstrap,
        ], [
            # arm_vm_init asserts that this node exists; TrustCache is optional.
            _node("memory-map", memory_map),
            *([_node("manifest-properties")] if userspace_fixture or root_boot else []),
        ]),
        _node("cpus", children=[_node(f"cpu{core}", [
            ("state", b"stopped\0" if core else b"running\0"),
            ("reg", struct.pack("<I", core)),  # MPIDR affinity, not its RES1 bit.
            ("device_type", b"cpu\0"),
            ("compatible", b"arm,cortex-a57\0"),
            ("timebase-frequency", struct.pack("<I", TIMEBASE_HZ)),
            *([("cluster-type", struct.pack("<I", 0)),
               ("die-cluster-id", struct.pack("<I", 0)),
               ("cluster-core-id", struct.pack("<I", core)),
               ("l2-cache-id", struct.pack("<I", 0))] if native_smp else []),
        ]) for core in range(4 if native_smp else 1)]),
        *([_node("psci", [("compatible", b"arm,psci-0.2\0"), ("method", b"smc\0")])]
          if native_smp else []),
        # Preserve XNU's RAM-based vnode sizing instead of the BCM2837 cap.
        _node("defaults", [("kern.maxvnodes", struct.pack("<I", ((4 << 30) if full_ram else MANAGED_SIZE) // 65536 + 1024))]
              if root_boot else []),  # No vmm-present on physical hardware.
        _node("options"),
        # Original AppleImage4 requires this node in the main OS environment;
        # the strings still identify the actual Nintendo board.
        *([_node("product", [("product-name", MODEL.encode("ascii") + b"\0"),
                             ("model", MODEL.encode("ascii") + b"\0")])]
          if userspace_fixture or root_boot else []),
        _node("arm-io", [
            ("device_type", b"nvidia,tegra210b01\0"),
            # Exactly the contiguous GICD/GICC span. The driver adds bus base
            # to these relative reg offsets; no wider Tegra bus is invented.
            ("ranges", struct.pack("<3Q", 0, GICD_BASE, GICC_BASE + GICC_SIZE - GICD_BASE)),
        ], [_node("intc", [
            ("compatible", b"arm,cortex-a15-gic\0"),
            ("reg", struct.pack("<4Q", 0, GICD_SIZE, GICC_BASE - GICD_BASE, GICC_SIZE)),
            # ArmCpu selects virtual timer INTID 27 internally. There is no
            # MMIO timer or Apple interrupt-controller="master" declaration.
        ])]),
    ])


def _source_record(relative: str) -> dict:
    data = read_regular(ROOT / relative)
    return {"path": relative, "size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def build(output: Path, *, userspace_fixture: bool = False, storage_probe: bool = False,
          root_boot: bool = False) -> dict:
    """Create a new local directory containing kernel.adt and its provenance."""
    output = safe_output(output)
    evidence = [_source_record(path) for path in EVIDENCE]
    consumers = [_source_record("out/native-a57/tree/" + path) for path in CONSUMERS]
    data = generate_adt(userspace_fixture=userspace_fixture, storage_probe=storage_probe,
                        root_boot=root_boot)
    bootstrap_profile = userspace_fixture or root_boot
    manifest = {
        "schema_version": 1,
        "profile": "native-oled-single-cpu-adt",
        "storage_probe": storage_probe,
        "storage_reference": ({"address": 0xA001F000, "size": 0x1000,
            "purpose": "same-boot firmware sector-zero reference; not block capacity or a storage driver"}
            if storage_probe else None),
        "native_xnu_boot_verified": False,
        "native_window_ownership_verified": False,
        "model": MODEL,
        "compatible": list(COMPATIBLE),
        "userspace_fixture": userspace_fixture,
        "root_boot": root_boot,
        "root_volume_uuid": ROOT_VOLUME_UUID if root_boot else None,
        "bootstrap_sidecars": {
            "NativeDataSeed" if root_boot else "RAMDisk": "zero address/size placeholder; requires a separate checked input",
            "TrustCache": "zero address/size placeholder; requires a separate checked input",
        } if bootstrap_profile else {},
        "os_environment": "main" if root_boot else "ramdisk" if userspace_fixture else None,
        "image4_hash_method": "sha2-384" if bootstrap_profile else None,
        "amfi_only_platform_code": True if bootstrap_profile else None,
        "platform_match": "ArmDTPlatform matches root name device-tree; no board compatibility patch",
        "cpu": {"count": 1, "compatible": "arm,cortex-a57", "affinity": 0,
                "timebase_frequency_hz": TIMEBASE_HZ,
                "provenance": "Native probe MIDR 0x411fd071, MPIDR 0x80000000; native EL1 CNTFRQ 19200000"},
        "gic": {"compatible": "arm,cortex-a15-gic", "version": 2,
                "distributor": {"start": GICD_BASE, "size": GICD_SIZE},
                "cpu_interface": {"start": GICC_BASE, "size": GICC_SIZE},
                "virtual_timer_intid": 27,
                "provenance": "Live OLED Linux DT resources; ArmCpu hardcodes virtual timer INTID 27",
                "native_interrupt_delivery_verified": False},
        "installed_dram": {"start": DRAM_BASE, "size": DRAM_SIZE,
                           "end": DRAM_BASE + DRAM_SIZE,
                           "provenance": "Native U-Boot banks plus excluded 3MiB tail; MC_EMEM_CFG=0x1000 reports 4096MiB",
                           "consumer": "arm_init gDramBase/gDramSize, not the allocator window"},
        "managed_memory_candidate": {"start": MANAGED_USER_BASE if bootstrap_profile else MANAGED_BASE,
                                     "size": MANAGED_SIZE,
                                     "end": (MANAGED_USER_BASE if bootstrap_profile else MANAGED_BASE) + MANAGED_SIZE,
                                     "consumer": "darwinboot-memory-size and separate loader boot_args",
                                     "ownership_verified": False},
        "random_seed": {"size_bytes": 64, "source": "host Python os.urandom at package generation",
                        "hardware_rng": False, "fresh_per_boot": False,
                        "limit": "Reusing this package reuses its seed; not production entropy or hardware attestation"},
        "nvram": {"kind": "empty host-created volatile CHRP v1 proxy", "bank_size": 8192,
                  "common_partition_size": 2048, "persistent": False, "controller": False,
                  "consumer": "ArmDTPlatform::hasNVRAMProxy and IODTNVRAM::initImageFormat"},
        "omitted": ["secondary CPUs", "PSCI node", "VMM flag", "PL011", "VirtIO", "UFS",
                    "native SD controller", "manifest identity or trust claims",
                    *(["RAMDisk"] if root_boot else [] if userspace_fixture else ["rootdisk", "RAMDisk", "TrustCache"]),
                    "unmeasured CPU/bus/memory clock frequencies", "Apple CPU implementation MMIO"],
        "limits": ["The loader must validate this candidate against runtime banks, reservations and exclusions before copying.",
                   "The hardware GIC security state and timer interrupt route remain unqualified.",
                   "This generator supplies no storage driver or sidecar contents; later AMFI/root-device initialization can fail.",
                   "Boot environment and SHA-384 are explicit development settings, not claims about Apple hardware identity or secure-boot state.",
                   "Display diagnostics use separately validated boot_args Video; no display hardware driver is described."],
        "evidence": evidence,
        "audited_consumers": consumers,
        "generator": _source_record("scripts/native_oled_adt.py"),
        "files": {"kernel.adt": {"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}},
    }
    output.mkdir(parents=True)
    (output / "kernel.adt").write_bytes(data)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory below workspace out")
    parser.add_argument("--userspace-fixture", action="store_true",
                        help="add paired RAMDisk/TrustCache placeholders and explicit RAM-disk boot metadata")
    parser.add_argument("--storage-probe", action="store_true",
                        help="describe the fixed external same-boot SD reference page; requires a bootstrap profile")
    parser.add_argument("--root-boot", action="store_true",
                        help="select the original OS by UUID with TrustCache and NativeDataSeed; requires --storage-probe")
    args = parser.parse_args()
    print(json.dumps(build(args.output, userspace_fixture=args.userspace_fixture,
                           storage_probe=args.storage_probe, root_boot=args.root_boot), indent=2))
