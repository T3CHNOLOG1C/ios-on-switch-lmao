# Native kernel handoff diagnostic and result

This is the next hardware check after the successful CPU probe and the photographed memory inventory. It tests U-Boot's kernel cleanup, direct framebuffer output, and an explicit transition from AArch64 EL2 to EL1. It does not load XNU or iPadOS.

## Observed result: EL1 established

The [v2 photograph](../out/switch/IMG_5513.png) shows native EL1 (`CurrentEL=4`), `SCTLR_EL1=0x30d00800`, all exception masks set, a reported counter frequency of 19,200,000 Hz and a positive 295,700-tick counter delta. Direct framebuffer output continued after firmware cleanup and after the EL1 transition. The runtime FDT has the two measured active RAM banks and a 48-byte memory property. [Full independent transcription and proof boundaries](../out/switch/native-handoff-v2-hardware-result.json).

The displayed FAILED banner is preserved in the evidence. The v2 checker expected `HCR_EL2=0xa0000000`, but the actual `0x80000000` is correct: Cortex-A57 implements EL3, making HCD bit29 reserved as zero. [Arm Cortex-A57 TRM DDI0488H, section4.3.34, table4-47](https://documentation-service.arm.com/static/5e906b9fc8052b1608760b6b). The source now writes and requires RW only (`0x80000000`). An EL3-present CPU model reproduces the old false failure and passes the corrected build. [Correction and model review](../out/research-native/handoff-hcr-review.json).

**No additional hardware run is needed for these observed milestones.** The corrected build is local only; the existing SD entry and historical result were preserved. Native XNU/iPadOS, interrupt delivery, SMP and a complete kernel allocation map remain unverified.

## Installation

V1 was installed and read back on 28 October 2026: eight new files totaling 777,756 bytes. All 41 previously tracked files remained byte-identical. [Installation receipt](../out/switch/native-handoff-v1-install.json), [exact-package review](../out/research-native/handoff-install-review.json).

The resulting photograph proves native EL2 execution and direct framebuffer output after firmware cleanup. V1 then refused with `0xD` because its memory-property validator omitted the firmware's unused third bank slot. It did not attempt EL1. [Observed result](../out/switch/native-handoff-v1-hardware-result.json). V2 was then installed and read back: eight new files, 777,788 bytes, with all 49 previously tracked files unchanged. The SD was safely ejected. [V2 installation receipt](../out/switch/native-handoff-v2-install.json). Its physical result is recorded above.

## Historical run procedure

This records how the evidence was captured; repeating it is not necessary:

1. Close Hekate USB storage.
2. Open **More Configs → IOSwitch Native Handoff v2**.
3. Photograph the whole result screen, including the bottom line. There is one result page.
4. Hold **POWER for at least 20 seconds** to turn the Switch off, then start Hekate normally.

The expected success line is **PASS: NATIVE EL1 EXECUTION AND READABLE TICKING COUNTER**. The screen should show entry EL `8` and EL1 CurrentEL `4`; those register encodings mean EL2 and EL1 respectively. All displayed register values are hexadecimal. Success proves this diagnostic's handoff and display path, not a native XNU boot.

If a refusal, exception, firmware logo or last numbered stage remains, photograph that instead. A blank screen is also an outcome to report; do not infer that the code returned or completed. The diagnostic has no automatic reboot. Its own refusal and exception paths hold indefinitely, but firmware faults before its exception vectors are installed can leave no readable report.

The local reproduction uses the pinned firmware memory-fixup functions: the old binary returns the same refusal code, while v2 reaches EL1. Additional cases reject nonzero extra-bank addresses/sizes, unsupported slot counts and wrong active banks, and verify the EL1 exception screen. [Seven-case model receipt](../out/research-native/handoff-cpu-model-v2-review.json), [firmware-shaped fixture](../out/research-native/handoff-inventory/postprep-memory-fixture.json). The later photograph supplies the physical EL1 evidence that these local runs cannot.

## What runs

The separate entry uses byte-identical copies of the console's existing ATF and U-Boot. Its script checks the OLED board identifiers and firmware fingerprint, confirms the inherited framebuffer description, then reads and verifies a bounded diagnostic image and the OLED device tree. The tree is copied to a separate RAM window and adjusted by the existing firmware's normal kernel preparation.

The firmware then performs its normal OS cleanup and calls the diagnostic. The diagnostic uses private stacks, checks the post-cleanup state and runtime RAM banks, draws directly into the inherited framebuffer, and enters EL1 with its MMU and caches off and exceptions masked. It reads the counter twice and holds the result. It does not enable interrupts or start secondary CPUs.

The existing firmware cleanup changes USB/UPHY controls and sends a GIC software interrupt. This is a kernel handoff, not a read-only register inspection. No SD storage-write, persistent environment update, partition change or eMMC write is authored by the diagnostic. The original Hekate, Android and Ubuntu entries are preserved.

## Evidence and limits

- [Physical memory inventory](native-memory-result.md): both runtime banks, framebuffer, firmware pointers and the active carveouts.
- [Firmware handoff audit](../out/research-native/handoff-inventory/bootm-handoff-source-audit.json): the pinned U-Boot preparation and cleanup path.
- [Source](../native/handoff/entry.S), [diagnostic](../native/handoff/probe.c), [display code](../native/handoff/display.h), and [package builder](../scripts/package_native_handoff.py).

Local CPU-model checks exercise the ARMv8.0 instructions, EL1 transition, parser refusal paths and exception screen. The model has synthetic RAM and a prepared caller; it does not model Tegra firmware, the OLED display controller or the physical boot path. The console photograph remains the required hardware evidence.

The fixed diagnostic region starts at `0xa0000000`, the staged image at `0xa0100000`, and the copied FDT at `0x8d000000`. Framebuffer writes are confined to `[0xf5a00000, 0xf5d84000)`. These bounded diagnostics do not establish a complete allocation map for XNU.
