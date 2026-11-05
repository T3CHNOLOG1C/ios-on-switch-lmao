#!/usr/bin/env python3
"""The kernel's boot-args: COMMAND_LINE, baked into the kernel window's boot_args (//boot:package).

    boot_args.py OUT [--smp] [--release]

writes the command line the package lays out (with --smp, cpus=1 becomes cpus=4 sheng-smp=1 ioswitch-smp=1; with
--release, the release profile's, without -v). Every
packaging step that depends on a boot-arg reads that file, not this module, so a comment edit here reruns only this
step. The kernel reads the line as one string of at most 1023 bytes (native_xnu_layout checks it).
"""
import sys
from pathlib import Path

COMMAND_LINE = ' '.join([
    '-v', 'serial=0', 'debug=0x14e', 'kt_fixed_zones=120', 'cpus=1', 'armcpu-self-ipi=1',
    'ioswitch-diag=3', 'sheng-pmgr=2', '-no_slto_panic',
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
    # 11-04 r11-nvdec: 'switch-hevc=1' dropped (14 bytes free). It only gated the overlay's VideoDecoders union, which is
    # now always shown: HEVCH8 (software HEVC in VideoToolbox's hardware slot, for HEIC) and AVD (H.264 on NVDEC, which
    # TegraNVDEC turns on by default; switch-nvdec=0 turns it off).
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
    # 11-05 r15-dvfs: switch-perf=1 now puts the CPU on L4T's DFLL with an on-demand governor (918-1785 MHz at the CVB
    # voltages, 1785 = Horizon's boost clock, never above; touch/frame-miss/launch boosts, the boot at the top) and falls
    # back to that ladder (1670 MHz) on any DFLL failure. switch-cpu-mode=pll keeps the fixed ladder, =dfll a fixed 1785.
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
    # GameController devices (drivers/SwitchJoyCon). 11-04: Joy-Con NFC dropped (user): switch-joycon-nfc=1 is off,
    # so joyconnfc no longer runs a 30 s MCU scan on every Joy-Con (R) attach.
    'switch-wlan=1', 'switch-joycon-rail=1',
    # 10-31 r4-bt: Bluetooth (drivers/SwitchBluetooth: BCM4356A3 on UART-D, patch RAM from the ADT, kernel control
    # com.apple.uart.bluetooth). Turns on, together: /arm-io/bluetooth + ioswitch,patchram here, the BlueTool stand-in
    # overlay, bluetoothd's BT_HCI_TRANSPORT (kern_exec.c) and drops com.apple.bluetoothd from the kernel-written
    # launchd disabled.plist. Never booted before. With it the smp line is 1023/1023 bytes.
    'switch-bt=1',
    # + display: CoreBrightness's slider to the panel (DCS 0x51), panel off/on with the render server's power requests
    # (display power), and the BH1730 ambient light sensor (drivers/SwitchBrightness).
    # 10-30 19:50: with these three (+ Metal) the boot hung before launchd (no log); off until tested one at a time.
    # 'switch-backlight=1', 'switch-sleep=1', 'switch-als=1',
    # 11-04 r10-sleep: display power (lock turns the panel, the DC scanout and the touch controller off) is on by default
    # in code now; 'switch-sleep=0' turns it off. switch-sleep=1 had no effect before launchd (it only wraps the render
    # server's IOMFB clients), so the 10-30 hang was the ALS or the then thread-timed panel commands.
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


# Four cores: the secondary A57s started by PSCI CPU_ON (the four-A57 ADT, adt_edit.four_cores).
SMP = ('cpus=1', 'cpus=4 sheng-smp=1 ioswitch-smp=1')


# Nothing in XNU or the drivers reads -v here: the kernel's console follows the loader's video profile (text mode).
VERBOSE = '-v'


def command_line(smp: bool, release: bool = False) -> str:
    """Return COMMAND_LINE, with four cores (smp), without -v (release)."""
    line = COMMAND_LINE.replace(*SMP) if smp else COMMAND_LINE
    if release:
        words = line.split(' ')
        words.remove(VERBOSE)
        line = ' '.join(words)
    return line


if __name__ == '__main__':
    flags = sys.argv[2:]
    if len(sys.argv) < 2 or not set(flags) <= {'--smp', '--release'} or len(set(flags)) != len(flags):
        raise SystemExit(__doc__)
    Path(sys.argv[1]).write_text(command_line('--smp' in flags, '--release' in flags) + '\n')
