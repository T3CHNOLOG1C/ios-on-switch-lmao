#!/usr/bin/env python3
"""Package a built kernel as the single "IOSwitch" Hekate boot entry.

Usage: package_raw_boot.py <build-dir> [--smp]
Writes out/boot/ioswitch/{switchroot/ioswitch/..., bootloader/ini/ioswitch.ini};
copy both trees over the SD root, replacing the previous entry. The entry is
self-contained and the command line is baked into the kernel window.
Also stages the userland overlay (tree/overlay/manifest.json) in out/boot/ioswitch/overlay/<absolute path>;
its contents go to /private/var/ioswitch/overlay/ on the Data volume (the deploy script copies them)."""
import hashlib
import json
import posixpath
import re
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_xnu_layout  # noqa: E402
from build_native_loader import (NATIVE_FULL_RAM_EXCLUSIONS, STAGE_SOURCE, STAMPS_BYTES,  # noqa: E402
                                 STAMPS_PROPERTY, build as build_loader, stage_into_bl33)
from package_native_handoff import legacy_image, LOAD  # noqa: E402

NAME = 'ioswitch'
WORK = ROOT / 'out/native-a57/root-v1'
INPUTS = WORK / 'passes/native-root-vmregions-v1/layout/manifest.json'
TEMPLATE = ROOT / 'boot/raw-v24-boot.cmd'   # last hardware-run raw script (//boot:package passes it in)
FIRMWARE = ROOT / 'bazel/pinned/firmware'   # bl31, bl33, oled-sku3.dtb, exporter.uimg, BCM4356A3.hcd (pinned)
APP_CRYPTEX_TRUST = ROOT / 'out/switch/cryptex-trustcaches/044-75188-038.dmg.trustcache'
# The TegraGM20B Metal plugin and the NVK library in its bundle (drivers/TegraGM20BMetal/build-tegra-gm20b-metal.py out/gpu);
# added when built. Stage the same build onto Data (stage-tegra-gm20b-metal.py): every rebuild changes both CDHashes.
GPU_TRUST = ROOT / 'out/gpu/TegraGM20BMetal.trustcache'
# Bluetooth patch RAM for drivers/SwitchBluetooth (boot-arg switch-bt=1): gitlab.com/switchroot/android/nx-firmware
# @ 521295deb9b3 CYW4356A3_001.004.009.0092.0095.bin ("nx HOS stock dump"), 38950 bytes, sha256 891c36e8bbfa...
BT_PATCH = FIRMWARE / 'BCM4356A3.hcd'
# Userland overlay: files shown over sealed system directories (IOSwitchRawMount.inc native_ioswitch_raw_overlay).
OVERLAY_MANIFEST = WORK / 'tree/overlay/manifest.json'
OVERLAY_ROOT = '/private/var/ioswitch/overlay'
# The kernel's display/touch mode (IOSWITCH_PORTRAIT_NATIVE's default there); the ADT must match it (portrait_native).
PORTRAIT_HEADER = WORK / 'tree/drivers/NativeMobileDisplay/IOSwitchPortrait.h'
# USB 2.0 device mode on the USB-C port (drivers/SwitchUSB): the USB function set configd's USBDeviceConfiguration
# plug-in gives Apple's IOUSBDeviceFamily, by name from /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist.
# The family goes on the bus only once every function of every configuration has a driver, so only sets whose
# drivers exist here work: 'unknownHardware' (AppleUSBMux + PTP: usbmuxd/Finder/Xcode/atc and ptpd photos) or
# 'standardMuxOnly' (AppleUSBMux alone, no userland function driver). J120's own stdMuxPTPEthValIDA needs a dozen
# function drivers this port lacks. None leaves /arm-io/usb-device out, and the driver then does nothing.
USB_DEVICE_CONFIGURATION = 'unknownHardware'
# USB 2.0 host mode on the same port (drivers/SwitchUSB SwitchXHCI): a keyboard, mouse or flash drive through a USB-C OTG
# adapter or an unpowered hub. True writes /arm-io/usb-xhci, the driver's switch; the role then follows the BM92T36.
USB_HOST = False
# The panel's colour gamut as iOS learns it: /product artwork-display-gamut, MobileGestalt's ArtworkDisplayGamut, whose
# HasExtendedColorDisplay is "== P3". 'sRGB' (what sRGB iPads carry) turns off deep colour everywhere: QuartzCore's
# CADeviceUseCIF10 (so the display reports no extended colours, and CADisplayMode, FBSDisplayConfiguration and UIScreen
# say sRGB), UIKit's _supportsDeepColor, and WebCore's screenSupportsExtendedColor, so WebKit tiles are BGRA8 rather than
# two-plane b3a8 and Core Animation stops making w30r surfaces, formats the GM20B only reaches through copies. 'P3' is
# J120's own. Asset catalog images that this IPSW ships only in P3 (some onboarding and tips art) are not found under
# sRGB. The kernel drops MobileGestalt's cache when this changes (kern_exec.c native_ioswitch_gamut_cache).
DISPLAY_GAMUT = 'sRGB'
COMMAND_LINE = ' '.join([
    '-v', 'serial=0', 'debug=0x14e', 'kt_fixed_zones=120', 'cpus=1', 'armcpu-self-ipi=1',
    'ioswitch-diag=1', 'sheng-pmgr=2', '-no_slto_panic',
    # The kernel ignores the two SD args now; the loader builder still gates on them.
    'ioswitch-sd-test=1', 'ioswitch-sd-files=1', 'ioswitch-root-boot=1',
    'offline-apfs-load=1', '-apfs_shared_datavolume', 'offline-mobile-graphics-load=1',
    'offline-legacy-register=1', 'offline-legacy-phase=4', 'offline-legacy-gop=1',
    'offline-legacy-unit-scale=1', 'offline-legacy-pm=1', 'offline-legacy-validate=1',
    'offline-legacy-transfer=1', 'offline-legacy-cpu-present=1', 'offline-key-events=1',
    'sheng-aks=1', 'cs_integrity_skip=1', 'ioswitch-storage=raw',
    # As UniXNU-qemu: scaled lock timeouts. (Its synthetic HID device needs a DriverKit dext that never
    # starts here and stalled boot; real touch/buttons will be in-kernel HID event services.)
    'ml-timeout-lock-panic-scale=40',
    # Display refresh tick: the GOP-style scanout has no vblank, so NativeLegacyDisplay sends the compositor's vsync.
    'sheng-vsync=60',
    # SD card: re-initialize into UHS-I SDR104 (1.8 V, ~200 MHz, tuned) instead of U-Boot's 3.3 V 48 MHz.
    # Remove to keep the inherited bus mode (NativeIOSwitchBlock.cpp, panics name the failing step).
    'ioswitch-sd-uhs=1',
    # HEIC/HEVC everywhere on the CPU: Apple's software HEVC decoder (VCPHEVC) also registered in VideoToolbox's hardware
    # slot, so hardware-required decodes (QuickLook previews, Photos thumbnails) work (overlay HEVCH8.videodecoder).
    'switch-hevc=1',
    # 10-30 07:35 boot froze at IOMobileFramebuffer start with no SwitchThermal/SwitchPerf line on screen: test without
    # the CPU raise (998 MHz / 800 mV as hekate leaves it).
    # 10-31 r3-cpu: the raise is staged (switch-perf-steps=N stops after step N; 0 = read and report only, no writes).
    # First hardware run: report only (speedo, M4_CFG, ramp, top rung).
    # 10-31 14:00 report run: CPU speedo 1659 id 0 process 1, MAX77812 v5 M4_CFG 0xd1, top 1683 MHz. Now the whole
    # ladder with the trace (a freeze shows the last "SwitchPerf trace:" step on screen).
    # 10-31 14:20: the full ladder froze at step 1 "cluster mux to PLLP (CCLKG 0x20004444)": the park source is wrong.
    # Off until r3-cpu fixes the park sequence.
    # 10-31 r3-cpu r2: park on PLLP_OUT4 (source 5) with the PLLP_OUT_CPU gate opened, as L4T. Test step 1 only.
    # 10-31 14:34: the whole ladder works: CPU 998 -> 1670 MHz at 1120 mV (speedo 1659, M4_CFG 0xd1 -> 0xf1). Trace off.
    'switch-perf=1',
    # Test ladder step 2: EMC (RAM) 204 -> 1600 MHz through the BPMP mailbox, as L4T does. 10-30: refused on every boot
    # ("needs VDD_CORE 1100 mV", a placeholder in hekate's B01 MTC tables); TegraBPMP.c now checks L4T's B01 table (675 mV).
    'switch-emc=1',
    # Test ladder step 3 (0900-integrated-emc booted to SpringBoard): audio. 10-30 15:55 panicked at the first stream start,
    # "I2S1 RX soft reset did not finish @SwitchAudio.cpp:554". Fixed (I2S master mode before the reset, as L4T), not yet
    # run: off while the GPU bring-up is tested on its own.
    # 10-30 17:03: back on with EMC at 1600 (confirmed) and the GPU off.
    'switch-audio=1', 'switch-audio-stats=1',
    # Test ladder step 4: GPU stage 4 (rail/ACR/GR/self-test, then AGXAccelerator with MetalPluginName TegraGM20BMetal), the
    # Metal bundle shown over /System/Library/Extensions from Data (deploy with STAGE_GPU=1).
    # 10-30 16:00 "tegra-gm20b: M1 FAIL pool -12": no contiguous 128 MiB below 4 GiB at runtime. The pool is now RAM the
    # kernel takes out of the VM at boot (arm_vm_init.c pmap_ioswitch_ram_init), above the bootstrap blocks (0xe2000000).
    # GPU bring-up ladder (one console line before each M1 step): 16:44 M1 powered the GPU, BOOT_0 12e000b1 (GM20B_B,
    # T210B01), refused as an unknown chipset; nouveau now maps 0x12e to gm20b like nvgpu. Stage 2 = + ACR/PMU/GR.
    # 10-30 16:52 stage 2: M1 ok (ring pass, GPCPLL 76.8 MHz), M2 ACR done but the first LTC access (0x17e338) times
    # out, then the PRI ring; GR init -110. Cause (agent, nvgpu gk20a_finalize_poweron): nvkm_device_init's fini pass
    # drops the GPU to the 1/31 bypass clock before L2/FB/ACR/PMU; gpu.c m2_device_init now keeps M1's GPCPLL up.
    # 17:50 M2 ok (gpc 1 tpc 2); M3 vm -22 = gm200 VMM bigpage 0 (must be 16/17), fixed. Stage 4 runs only if M3 passes.
    # 18:27 stage 4: Metal device up in backboardd, then CA aborts in create_pipeline_state (metal2vulkan refuses a colour
    # attachment format, NotImplemented). Metal off until the bridge is fixed; stage 3 with the clock raise to 460.8 MHz.
    # 19:30 bridge patch 0004 (unordered extended-range blend; CA warms BGR10_XR/RGB10A8_2P_XR10 pipelines): Metal back on.
    # 20:00 gpu4-xr: backboardd stays up, pipelines compile, but every IOSurface import is refused (HostMemory
    # NotRepresentable) so nothing composites. Metal off until the bridge imports surfaces.
    # 20:30 NVK host-pointer import (IOSurfaces) fixed: Metal back on; iconservicesagent is granted the GPU too
    # (NativeSandboxGrantProcess), so the switch-no-iconagent stopgap is off. Never turn it back on: the 2013 boot showed
    # that a refused exec leaves launchd holding SpringBoard's first icon request forever (no first frame in 3 hours).
    'tegra-gpu=4', 'sheng-gpu-addon=1', 'tegra-gpu-mhz=768', 'tegra-gpu-pool-pa=0xe4000000',
    # 10-30 18:45 feature test 1 (built 10-29/30, never run on hardware): Wi-Fi (drivers/SwitchWLAN), rail Joy-Cons as
    # GameController devices and Joy-Con (R) NFC (drivers/SwitchJoyCon, joyconnfc).
    'switch-wlan=1', 'switch-joycon-rail=1', 'switch-joycon-nfc=1',
    # 10-31 r4-bt: Bluetooth (drivers/SwitchBluetooth: BCM4356A3 on UART-D, patch RAM from the ADT, kernel control
    # com.apple.uart.bluetooth). Turns on, together: /arm-io/bluetooth + ioswitch,patchram here, the BlueTool stand-in
    # overlay, bluetoothd's BT_HCI_TRANSPORT (kern_exec.c) and drops com.apple.bluetoothd from the kernel-written
    # launchd disabled.plist. Never booted before. With it the smp line is 1023/1023 bytes.
    'switch-bt=1',
    # + display: CoreBrightness's slider to the panel (DCS 0x51), panel off/on with the render server's power requests
    # (display power only; system sleep is switch-sleep=2), and the BH1730 ambient light sensor (drivers/SwitchBrightness).
    # 10-30 19:50: with these three (+ Metal) the boot hung before launchd (no log); off until tested one at a time.
    # 'switch-backlight=1', 'switch-sleep=1', 'switch-als=1',
    # 10-31: two boots died (panel noise) while the user moved the brightness slider: TegraPanel's DSI command sequence
    # runs late in the frame (thread-context frame-end wakeups). Off until it runs from the DC frame-end filter.
    # 10-31 r3-panel: the DSI command steps now run in the DC A frame-end filter (TegraPanel), one per FRAME_END,
    # with a V_BLANK guard before the syncpoint reset. Back on. If the noise returns: switch-backlight-syncpt=0.
    'switch-backlight=1',
    # 10-31 display path (r3-fps): a stats line every 5 s (swaps/s, per-stage us, GPU wait timeouts), a 64-bit swizzle
    # loop (~2-3 ms instead of ~16 ms per frame) and kept per-surface kernel mappings. Async copy (ioswitch-display-async)
    # stays off until these are seen working.
    # ioswitch-surface-maps dropped 10-31 (map cost measured 0-40 us; boot-args were over 1024 characters).
    # ioswitch-fast-swizzle dropped 10-31: the copy is read-bound (source Device memory), the fast loop gained nothing.
    'ioswitch-display-stats=1',
    # 10-31 r2: the block-linear scanout views were probably device memory (copy store-bound at ~77 ns per 16 B);
    # write-combine set at descriptor creation, as the GPU pool does.
    'ioswitch-scanout-wc=1',
    # 10-31 r3-fps r3: double-buffered flips at frame end (no tearing) and the copy off the gate, retired at hand-off.
    'ioswitch-scanout-double=1', 'ioswitch-display-async=2',
    # 10-31 r3-gpu r3: the copy engine converts each frame into the back scanout buffer (tegra_gm20b_scanout_copy).
    'ioswitch-gpu-copy=1',
    # 10-31: sheng-early-retire=0 would stop the tearing by making CA wait for each copy; rejected (slower). The tearing is
    # fixed in GPU order instead (user execs acquire the last present copy's fence).
    # 10-31 GPU (r3-gpu): imports cleaned at import, read-only imports accepted, and every GPU failure (nvkm interrupt
    # report, channel kill, sync_for_cpu over ioswitch-gpu-wait-ms) panics with one line. Stats line every 5 s (pool use,
    # fence latency, MC errors). tegra-gpu-import-sync=1 (clean every import at each submit) only if the noise returns.
    # 10-31 r2: user binds L2-cacheable (vol=0) with nvgpu-style maintenance: L2 invalidate before each exec, WFI +
    # FB_FLUSH + L2_FLUSH_DIRTY before each fence. M3 self-test goes through a cached mapping (fails closed).
    'tegra-gpu-stats=1', 'tegra-gpu-l2=1',
    # 10-31 r3-panic: panic screen + record in hekate's L4T pstore RAM (0xb0000000), reboot 15 s after the panic, written
    # to Data /private/var/ioswitch/panic-<UTC>.log on the next boot. (tegra-gpu-pool-mb=128 dropped: it is the default.)
    'ioswitch-panic-save=1',
    # ioswitch-proclog=1 prints one console line per process exec and exit. Off: userland logs now come from the
    # Data volume, and every console line costs a rotated repaint of the text console during boot.
])


def adt_parse(data, pos=0):
    count, children = struct.unpack_from('<II', data, pos)
    pos += 8
    props, nodes = [], []
    for _ in range(count):
        name = data[pos:pos + 32].split(b'\0', 1)[0].decode()
        word = struct.unpack_from('<I', data, pos + 32)[0]
        size = word & 0x7fffffff
        props.append([name, word, data[pos + 36:pos + 36 + size]])
        pos += 36 + size + (-size % 4)
    for _ in range(children):
        node, pos = adt_parse(data, pos)
        nodes.append(node)
    return [props, nodes], pos


def adt_serialize(node):
    props, children = node
    out = struct.pack('<II', len(props), len(children))
    for name, word, value in props:
        out += name.encode().ljust(32, b'\0') + struct.pack('<I', word) + value + bytes(-len(value) % 4)
    return out + b''.join(adt_serialize(c) for c in children)


def put(node, name, value):
    for prop in node[0]:
        if prop[0] == name:
            prop[1:] = [(prop[1] & 0x80000000) | len(value), value]
            return
    node[0].append([name, len(value), value])


def child(node, name):
    for c in node[1]:
        if any(p[0] == 'name' and p[2].split(b'\0')[0] == name.encode() for p in c[0]):
            return c
    c = [[['name', len(name) + 1, name.encode() + b'\0']], []]
    node[1].append(c)
    return c


def u32(value):
    return struct.pack('<I', value)


def four_cores(tree):
    """All four A57s (MPIDR 0-3, one cluster, shared L2) started by PSCI CPU_ON over SMC."""
    cpus = child(tree, 'cpus')
    for core in range(4):
        cpu = child(cpus, f'cpu{core}')
        put(cpu, 'reg', u32(core))
        put(cpu, 'device_type', b'cpu\0')
        put(cpu, 'compatible', b'arm,cortex-a57\0')
        put(cpu, 'timebase-frequency', u32(19200000))
        if core:
            put(cpu, 'state', b'stopped\0')
        for key, value in (('cluster-core-id', core), ('cluster-type', 0), ('die-cluster-id', 0), ('l2-cache-id', 0)):
            put(cpu, key, u32(value))
    psci = child(tree, 'psci')
    put(psci, 'compatible', b'arm,psci-0.2\0')
    put(psci, 'method', b'smc\0')


def im4p_payload(data):
    """The payload of an IM4P (SEQUENCE { "IM4P", type, description, OCTET STRING payload, ... })."""
    def header(at):
        tag, length, at = data[at], data[at + 1], at + 2
        if length & 0x80:
            count = length & 0x7f
            length, at = int.from_bytes(data[at:at + count], 'big'), at + count
        return tag, at, length
    _, at, length = header(0)
    end, fields = at + length, []
    while at < end:
        tag, start, length = header(at)
        fields.append((tag, data[start:start + length]))
        at = start + length
    if fields[0][1] != b'IM4P':
        raise SystemExit('not an IM4P')
    return next(value for tag, value in fields if tag == 0x04)


def trust_cache_bundle(bundle, *modules):
    """`bundle` (u32 count, u32 offsets, version-2 modules) with `modules` appended."""
    count = struct.unpack_from('<I', bundle)[0]
    starts = struct.unpack_from(f'<{count}I', bundle, 4)
    existing = [bundle[s:e] for s, e in zip(starts, (*starts[1:], len(bundle)))]
    merged = existing + [m for m in modules if m not in existing]
    offset, offsets = 4 + 4 * len(merged), []
    for module in merged:
        offsets.append(offset)
        offset += len(module)
    return struct.pack(f'<I{len(merged)}I', len(merged), *offsets) + b''.join(merged)


def cdhash(image):
    """The CDHash of a thin Mach-O's embedded signature: the first 20 bytes of the SHA-256 of its SHA-256
    CodeDirectory (what codesign -dvvv prints as CDHash and what a trust cache entry holds)."""
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', image)
    if magic != 0xfeedfacf:
        raise SystemExit('overlay: not a thin 64-bit Mach-O')
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', image, at)
        if cmd == 0x1d:                                         # LC_CODE_SIGNATURE
            offset = struct.unpack_from('<I', image, at + 8)[0]
            blob_magic, _, count = struct.unpack_from('>III', image, offset)
            if blob_magic != 0xfade0cc0:                        # embedded signature SuperBlob
                break
            for index in range(count):
                slot, start = struct.unpack_from('>II', image, offset + 12 + 8 * index)
                if slot != 0 and not 0x1000 <= slot < 0x1005:   # CodeDirectory and its alternates
                    continue
                directory = offset + start
                cd_magic, length = struct.unpack_from('>II', image, directory)
                if cd_magic == 0xfade0c02 and image[directory + 37] == 2:   # hashType SHA-256
                    return hashlib.sha256(image[directory:directory + length]).digest()[:20]
        at += size
    raise SystemExit('overlay: no SHA-256 CodeDirectory (sign it with codesign -s -)')


def trust_module(hashes):
    """A version-2 trust cache module of sorted (CDHash, SHA-256, flags 0, launch-constraint category 0) entries.
    Category 0 because overlay code lives on Data: the system categories' volume constraints would refuse it."""
    entries = b''.join(digest + bytes([2, 0, 0, 0]) for digest in sorted(set(hashes)))
    return struct.pack('<I16sI', 2, hashlib.sha256(entries).digest()[:16], len(entries) // 24) + entries


def load_overlay():
    """tree/overlay/manifest.json: {"files": [{"path": absolute, "source": relative to the tree, "trust": bool,
    "mode": "0755", "boot-arg": name, "union": absolute}]}. Returns the files to stage, the directories the kernel
    unions (each with the boot-arg that gates it, shared by all its files) and the CDHashes of the trusted ones. The
    union directory is the file's own unless "union" names an existing ancestor (XNU unions are deep, so a new bundle
    directory can appear inside a sealed one). A source not built yet is left out with a warning. "portrait": true
    stages the file only for a portrait-native kernel (portrait_native); "unless-boot-arg": name leaves it out when
    COMMAND_LINE sets that boot-arg (boot_arg_on)."""
    manifest = json.loads(OVERLAY_MANIFEST.read_text()) if OVERLAY_MANIFEST.exists() else {'files': []}
    files, gates, hashes = [], {}, []
    for entry in manifest['files']:
        if entry.get('portrait') and not portrait_native():
            continue
        if entry.get('unless-boot-arg') and boot_arg_on(entry['unless-boot-arg']):
            continue
        path = entry['path']
        if not path.startswith('/') or posixpath.normpath(path) != path or path.startswith('/private/var/'):
            raise SystemExit(f'overlay: bad path {path}')
        directory, gate = entry.get('union', posixpath.dirname(path)), entry.get('boot-arg', '')
        if not path.startswith(directory.rstrip('/') + '/'):
            raise SystemExit(f'overlay: {path} is not inside its union {directory}')
        if gates.setdefault(directory, gate) != gate:
            raise SystemExit(f'overlay: files in {directory} need the same boot-arg')
        source = WORK / 'tree' / entry['source']
        if not source.is_file():
            print(f'overlay: {path}: {source} not built, left out')
            continue
        data = source.read_bytes()
        digest = cdhash(data) if entry.get('trust') else None
        if digest:
            hashes.append(digest)
        files.append((path, data, int(entry.get('mode', '0755' if digest else '0644'), 8), digest, directory))
    used = {directory for *_, directory in files}
    files = [entry[:4] for entry in files]
    return files, sorted((d, g) for d, g in gates.items() if d in used), hashes


def boot_arg_on(name):
    """True when COMMAND_LINE sets `name` to a non-zero value."""
    return any(arg.startswith(name + '=') and arg.split('=', 1)[1] not in ('0', '') for arg in COMMAND_LINE.split())


def portrait_native():
    """True when the kernel is built portrait-native: IOSwitchPortrait.h's default for IOSWITCH_PORTRAIT_NATIVE."""
    found = re.search(r'^#define IOSWITCH_PORTRAIT_NATIVE ([01])\b', PORTRAIT_HEADER.read_text(), re.M)
    if not found:
        raise SystemExit(f'{PORTRAIT_HEADER}: no IOSWITCH_PORTRAIT_NATIVE default')
    return found.group(1) == '1'


def ipad_adt(data, smp=False, overlay_dirs=()):
    """The inherited Switch tree plus what iPadOS userland expects of a J120
    (values of UniXNU-qemu's make-qemu-adt.py, which boots this build to SpringBoard)."""
    tree, end = adt_parse(data)
    if end != len(data):
        raise SystemExit('trailing bytes after the ADT')
    put(tree, 'model', b'iPad7,1\0')            # hw.model; idle code matches "compatible"
    put(tree, 'target-type', b'J120\0')
    # The audio server keys its routing database on the board name; the Switch entries stay
    # after it (ioswitch_idle_board looks for nintendo,aula).
    compatible = next(p[2] for p in tree[0] if p[0] == 'compatible')
    if not compatible.startswith(b'J120AP\0'):
        put(tree, 'compatible', b'J120AP\0' + compatible)
    chosen = child(tree, 'chosen')
    put(chosen, 'ephemeral-storage', u32(0))    # Data is a persistent APFS volume
    # Portrait-native kernel (IOSwitchPortrait.h): iOS is given the 720x1280 panel and runs landscape with the panel's
    # top on the user's right. MobileGestalt DisplayBootRotation reads this: SpringBoard starts in interface orientation
    # 4 (landscape left, -[SpringBoard _bootOrientation]) and ProgressUI turns the boot Apple logo the same way. The
    # kernel also restores SpringBoard's rotation lock to 4 when it is set (kern_exec.c). display-rotation stays 0
    # (published at runtime): a non-zero one would mean a panel mounted rotated, and iOS would composite sideways.
    if portrait_native():
        put(chosen, 'display-boot-rotation', u32(270))
    # Locally administered addresses; MobileGestalt waits on these when they are missing.
    for index, key in enumerate(('mac-address-wifi0', 'mac-address-bluetooth0', 'mac-address-ethernet0')):
        put(chosen, key, bytes([0x02, 0x1d, 0x5a, 0x00, 0x00, index + 1]))
    for key in ('certificate-production-status', 'effective-production-status-ap'):
        put(chosen, key, u32(0))
    for key in ('certificate-security-mode', 'effective-security-mode-sep'):
        put(chosen, key, u32(1))
    # System sleep only with SwitchSuspend (switch-sleep=2: PSCI SYSTEM_SUSPEND to SC7); otherwise IOPMrootDomain
    # never sleeps (gSleepDisabledFlag), since ArmCpu has no other way to stop a core.
    put(child(tree, 'defaults'), 'sleep-disabled', u32(0 if 'switch-sleep=2' in COMMAND_LINE.split() else 1))
    product = child(tree, 'product')
    for key, value in (('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                       ('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                       ('model', b'iPad7,1\0'), ('artwork-device-idiom', b'pad\0'),
                       ('artwork-device-subtype', u32(2732)), ('artwork-scale-factor', u32(2)),
                       ('artwork-display-gamut', DISPLAY_GAMUT.encode() + b'\0'),
                       ('compatible-device-fallback', b'iPad6,7\0'),
                       ('thin-bezel', u32(1)),
                       ('product-id', bytes.fromhex('e9fefef6e6f9d12a5ba1a2551866cbbd749f0ec3'))):
        put(product, key, value)
    put(child(product, 'audio'), 'wireless-splitter', u32(1))   # as UniXNU's 21H461 profile
    # Bluetooth (drivers/SwitchBluetooth, switch-bt=1). ArmDTPlatform publishes every /arm-io child, so this is the
    # IOService named "bluetooth" bluetoothd looks up (local-mac-address, transport-speed, transport-encoding; 1 = H4,
    # J120's 3 = H4BC would also open Apple's /dev/btwake). The kernel downloads ioswitch,patchram itself, since
    # BlueTool has no BCM4356A3 firmware, and replaces local-mac-address (and /chosen mac-address-bluetooth0) with the
    # unit's own address from the fuse lot code when it can read it.
    # Only when the boot-arg is in COMMAND_LINE: without the radio, the node would only change what wifid and
    # MobileGestalt see on an otherwise unchanged boot (toggling a boot-arg repackages anyway).
    if boot_arg_on('switch-bt'):
        bluetooth = child(child(tree, 'arm-io'), 'bluetooth')
        for key, value in (('device_type', b'bluetooth\0'), ('local-mac-address', bytes([0x02, 0x1d, 0x5a, 0x00, 0x00, 0x02])),
                           ('transport-speed', u32(3000000)), ('transport-encoding', u32(1))):
            put(bluetooth, key, value)
        if BT_PATCH.exists():
            put(bluetooth, 'ioswitch,patchram', BT_PATCH.read_bytes())
    # Wi-Fi (drivers/SwitchWLAN, switch-wlan=1): /arm-io/wlan as J120 has it. With the boot-arg the kernel replaces
    # local-mac-address (and /chosen mac-address-wifi0) with the unit's own address from the fuse lot code, the one
    # the chip is given, before userland starts.
    if boot_arg_on('switch-wlan'):
        wlan = child(child(tree, 'arm-io'), 'wlan')
        for key, value in (('device_type', b'wlan\0'), ('local-mac-address', bytes([0x02, 0x1d, 0x5a, 0x00, 0x00, 0x01]))):
            put(wlan, key, value)
        # The platform UUID. IOPlatformExpertDevice::generatePlatformUUID makes IOPlatformUUID from /chosen
        # unique-chip-id (8 bytes) and chip-id (4 bytes), as iBoot sets them; without them the IOPlatformUUID resource
        # is never published and gethostuuid() with no timeout waits for ever. configd's IPConfiguration start() makes
        # one call (its CGA host UUID, as net.inet6.send.opmode is 1), so configd's plugin thread stops there: no other
        # start(), no prime(), so State:/Network/Interface never lists en0 and wifid makes no Wi-Fi device. chip-id is
        # J120's A10X (T8011); unique-chip-id stands in for the ECID, a fixed value with the overlay MACs' 02:1d:5a.
        put(chosen, 'chip-id', u32(0x8011))
        put(chosen, 'unique-chip-id', struct.pack('<Q', 0x001d5a0000000001))
    # USB device mode (drivers/SwitchUSB): /arm-io/usb-device with an iPad usb-device node's identity (J120: Apple
    # 0x05ac:0x12ab "iPad"), published by ArmDTPlatform; SwitchXUDC starts on it and IOUSBDeviceFamily reads these.
    # compatible is not usb-device,t8011, so nothing of Apple's matches the node itself.
    if USB_DEVICE_CONFIGURATION:
        usb = child(child(tree, 'arm-io'), 'usb-device')
        for key, value in (('compatible', b'usb-device,tegra210\0'), ('device_type', b'usb-device\0'),
                           ('vendor-id', u32(0x05ac)), ('product-id', u32(0x12ab)), ('product-version', u32(0x0701)),
                           ('bcd-usb-version', u32(0x0200)), ('vendor-string', b'Apple Inc.\0'),
                           ('manufacturer-string', b'Apple Inc.\0'), ('product-string', b'iPad\0'),
                           ('configuration-string', USB_DEVICE_CONFIGURATION.encode() + b'\0')):
            put(usb, key, value)
    # USB host mode: /arm-io/usb-xhci (J120 has one), present = on. compatible is not usb-xhci,t8011, so nothing of
    # Apple's matches the node itself; SwitchXHCI makes its own provider with the register window and interrupt.
    if USB_HOST:
        xhci = child(child(tree, 'arm-io'), 'usb-xhci')
        for key, value in (('compatible', b'usb-xhci,tegra210\0'), ('device_type', b'usb-xhci\0')):
            put(xhci, key, value)
    # Directories the kernel shows the Data overlay over: NUL-terminated "<directory>[:<boot-arg>]".
    if overlay_dirs:
        put(chosen, 'ioswitch-overlay', b''.join(f'{d}:{g}\0'.encode() if g else f'{d}\0'.encode()
                                                 for d, g in overlay_dirs))
    buttons = child(tree, 'buttons')
    for key, value in (('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                       ('button-names', b'hold\0voldown\0volup\0'), ('home-button-type', u32(2))):
        put(buttons, key, value)
    # APFS's no-SEP mode; it also permits the unencrypted Data volume.
    put(child(tree, 'filesystems'), 'DoNotUseSEP', u32(1))
    # Pre-kernel boot timing (native/loader/stamps.h): zeros here, filled by the loader, logged once by XNU.
    put(chosen, STAMPS_PROPERTY, bytes(STAMPS_BYTES))
    if smp:
        four_cores(tree)
    return adt_serialize(tree)


# The hekate entry. Every key also reaches U-Boot's environment (hekate's L4T launcher passes the section on; U-Boot
# imports it before its distro scan of boot_prefixes).
# bootwait=0: hekate always shows its boot logo before an L4T entry and, launched from Nyx, waits [config] bootwait
# seconds on it (3 by default; the entry's own key wins, main.c _auto_launch). Holding VOL- still opens hekate's menu.
# scan_dev_for_extlinux: the distro scan's extlinux.conf probe (this entry has none) becomes the first boot timing
# sample, taken once U-Boot, the SD card and hekate's env are up (native/loader/stamps.h).
# bootcmd_mmc0=true: should boot.scr not take over, U-Boot's distro loop goes on to mmc0, the eMMC (initialize, read
# its partition table, scan it); never. bl33_patch also drops mmc0 from boot_targets for failures before hekate's
# env is imported (an SD card U-Boot cannot read).
HEKATE_ENTRY = ('[IOSwitch]\nl4t=1\nboot_prefixes=/switchroot/{name}/\nid=IOSWITCH\nbootwait=0\n'
                'scan_dev_for_extlinux=setexpr.l ios_uboot *0x60005010\nbootcmd_mmc0=true\n')
# U-Boot's built-in default environment in switchroot's bl33.bin (env/common.c default_environment, imported whole at
# start: a run of "name=value\0" ended by an empty string), rewritten in place to the same size so nothing else in the
# binary moves:
#   * boot_targets: SD (mmc1) only, so U-Boot never falls back to the eMMC, even when it cannot read the SD card and
#     hekate's env (bootcmd_mmc0=true above) is never imported; bootcmd_mmc0 itself goes.
#   * two boot timing samples (native/loader/stamps.h) in what runs before hekate's env: bootcmd's start (U-Boot
#     initialized) and mmc_boot once `mmc dev 1` has brought the SD card up.
#   * room for them: four variables nothing on this boot path reads (PXE, ramdisk and DHCP script addresses).
ENV_START = b'\0bootcmd=run distro_bootcmd\0'
ENV_EDITS = {
    b'bootcmd=run distro_bootcmd': b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd',
    b'mmc_boot=if mmc dev ${devnum}; then setenv devtype mmc; run scan_dev_for_boot_part; fi':
        b'mmc_boot=if mmc dev ${devnum}; then setexpr.l ios_u_sd *0x60005010; setenv devtype mmc; '
        b'run scan_dev_for_boot_part; fi',
    b'boot_targets=mmc1 mmc0 ': b'boot_targets=mmc1',
}
ENV_DROPS = (b'pxefile_addr_r=0x8ff00000', b'ramdisk_addr_r=0x92000000', b'boot_script_dhcp=boot.scr.uimg',
             b'bootcmd_mmc0=setenv devnum 0; run mmc_boot')


def bl33_patch(bl33: bytes) -> bytes:
    """switchroot's U-Boot with its default environment as ENV_EDITS / ENV_DROPS say; every other byte unchanged."""
    if bl33.count(ENV_START) != 1:
        raise SystemExit('bl33.bin: U-Boot default environment not found exactly once')
    start = bl33.index(ENV_START) + 1
    end = bl33.index(b'\0\0', start) + 1  # the empty string that ends it
    entries = bl33[start:end - 1].split(b'\0')
    if any(entries.count(key) != 1 for key in (*ENV_EDITS, *ENV_DROPS)):
        raise SystemExit('bl33.bin: U-Boot default environment is not the one this patch was written for')
    entries = [ENV_EDITS.get(e, e) for e in entries if e not in ENV_DROPS]
    block = b'\0'.join(entries) + b'\0\0'
    if len(block) > end + 1 - start:
        raise SystemExit('bl33.bin: patched U-Boot environment does not fit')
    return bl33[:start] + block.ljust(end + 1 - start, b'\0') + bl33[end + 1:]


def boot_script(text: str, loader_bytes: int, bl33: bytes, constants: dict, staged_bytes: int) -> str:
    """boot.cmd from the template (TEMPLATE): this entry's paths, the loader's size, and bl33.bin's size, header and
    trailer words for the staged kernel window (native/loader/stage.h)."""
    prefix = '/switchroot/' + NAME + '/'
    for old in ('/switchroot/ioswitch-native-xnu-v23/', '/switchroot/ioswitch-native-xnu-v10/'):
        text = text.replace(old, prefix)
    text = text.replace('0x1c040', hex(loader_bytes))
    trailer = STAGE_SOURCE + staged_bytes
    for name, value in (('BL33_BYTES', len(bl33)), ('STAGE_STREAM', constants['STREAM_BYTES']),
                        ('STAGE_BYTES', staged_bytes), ('STAGE_ID_LOW', constants['STAGE_ID'] & 0xFFFFFFFF),
                        ('STAGE_ID_HIGH', constants['STAGE_ID'] >> 32),
                        *((f'TRAILER_{i}', trailer + 4 * i) for i in range(6))):
        if f'@{name}@' not in text:
            raise SystemExit(f'boot script template has no @{name}@')
        text = text.replace(f'@{name}@', hex(value))
    # The command line now lives in the layout; drop the fixed-address args patch.
    text, removed = re.subn(r'\n# Select raw only.*?\nfi\n', '\n', text, flags=re.S)
    if (removed != 1 or re.search(r'/switchroot/(?!' + NAME + '/)', text) or 'raw-args' in text or
            'crc32 -v' in text or '@' in text or 'kernel-window.bin' in text):
        raise SystemExit('boot script template did not match')
    return text


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--smp'):
        raise SystemExit(__doc__)
    build, smp = Path(sys.argv[1]).resolve(), len(sys.argv) == 3
    command_line = COMMAND_LINE.replace('cpus=1', 'cpus=4 sheng-smp=1 ioswitch-smp=1') if smp else COMMAND_LINE
    sources = json.loads(INPUTS.read_text())['sources']
    staging = build / 'package'
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    overlay_files, overlay_dirs, overlay_hashes = load_overlay()
    adt = staging / 'ioswitch.adt'
    adt.write_bytes(ipad_adt(Path(sources['adt']['path']).read_bytes(), smp, overlay_dirs))
    if ('/filesystems', 'DoNotUseSEP') not in native_xnu_layout.parse_adt(adt.read_bytes()).properties:
        raise SystemExit('ADT patch failed')
    # The static bundle covers the root and the OS cryptex; the App cryptex (Safari,
    # webbookmarksd, ...) brings its own trust cache, which ignition would load on iOS;
    # the overlay's own code gets a module of its own.
    trust = staging / 'trustcache.bundle'
    modules = [im4p_payload(APP_CRYPTEX_TRUST.read_bytes())] + ([trust_module(overlay_hashes)] if overlay_hashes else [])
    if GPU_TRUST.is_file():
        modules.append(GPU_TRUST.read_bytes())
    trust.write_bytes(trust_cache_bundle(Path(sources['TrustCache']['path']).read_bytes(), *modules))
    layout = native_xnu_layout.build_layout(
        build / 'kernel.macho', adt, staging / 'layout',
        physical_base=0xb207c000, window_size=768 << 20,
        reserved_ranges=[{'name': n, 'start': s, 'size': z} for n, s, z in NATIVE_FULL_RAM_EXCLUSIONS],
        command_line=command_line, trustcache_path=trust,
        data_seed_path=Path(sources['NativeDataSeed']['path']),
        video=[0xf5a00000, 0, 2880, 720, 1280, 0x320], full_ram=True)
    built = build_loader(staging / 'layout', staging / 'adapter', native_experiment=True,
                         storage_probe=True, storage_files=True, root_boot=True)
    loader = legacy_image((staging / 'adapter/loader.bin').read_bytes(), os_type=5, kind=2,
                          name='Loader', load=LOAD, entry=LOAD)
    # Packed by the loader builder (its nonzero extents, LZ4-compressed, back to back; the loader decodes it into
    # XNU's window). It rides in bl33.bin after U-Boot (native/loader/stage.h): hekate reads it at SDR104.
    window = (staging / 'adapter/kernel-window.bin').read_bytes()
    constants = built['constants']
    if constants['SOURCE'] != STAGE_SOURCE or constants['PACKED_BYTES'] != len(window):
        raise SystemExit('loader was not built for the bl33-staged window')
    bl33 = stage_into_bl33(bl33_patch((FIRMWARE / 'bl33.bin').read_bytes()), window, constants['STREAM_BYTES'])

    text = boot_script(TEMPLATE.read_text(), len(loader), bl33, constants, len(window))
    script = text.encode('ascii')
    scr = legacy_image(struct.pack('>II', len(script), 0) + script, os_type=17, kind=6, name='Boot')

    out = ROOT / 'out/boot' / NAME
    if out.exists():
        shutil.rmtree(out)
    files = {f'switchroot/{NAME}/{n}': (FIRMWARE / n).read_bytes()
             for n in ('bl31.bin', 'oled-sku3.dtb', 'exporter.uimg')}
    files.update({
        f'switchroot/{NAME}/bl33.bin': bl33,
        f'switchroot/{NAME}/loader.uboot.bin': loader,
        f'switchroot/{NAME}/boot.cmd': script,
        f'switchroot/{NAME}/boot.scr': scr,
        f'bootloader/ini/{NAME}.ini': HEKATE_ENTRY.format(name=NAME).encode(),
    })
    for relative, data in files.items():
        (out / relative).parent.mkdir(parents=True, exist_ok=True)
        (out / relative).write_bytes(data)
    for path, data, mode, _ in overlay_files:
        target = out / 'overlay' / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
    print(f'{out}: {len(files)} files, {sum(map(len, files.values()))} bytes; '
          f'kernel window {len(window)} bytes, entry {layout["entry_pa"]:#x}')
    print(f'overlay: {len(overlay_files)} files ({sum(1 for *_, digest in overlay_files if digest)} trusted) '
          f'-> Data {OVERLAY_ROOT}')


if __name__ == '__main__':
    main()
