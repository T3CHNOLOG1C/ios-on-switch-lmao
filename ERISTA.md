# Erista (the 2017 Switch, T210)

Work in progress on the `erista` branch. Upstream supports the OLED (Aula, T210B01) only.

`--config=erista` (`--//bazel:board=icosa`) now picks the board for what was hardcoded to the OLED: the board ADT's
model/compatible/arm-io SoC (`tools/package/native_oled_adt.py` BOARDS), U-Boot's board test
(`boot/u-boot-script.txt`, `tools/package/boot_script.py` BOARD_TESTS: hekate's sku 0 on a T210), the emulator's
fuse page (`tools/emu/hekate.py`), and the kernel's idle-board match (`osfmk/arm64/cpu.c` accepts `nintendo,icosa`).

Decided at run time from APB_MISC_GP_HIDREV (`kits/SwitchChip`): TegraSE and the AppleKeyStore probe use SE1 only
on a T210; the loader's seed takes its DRBG output from SE1 (AHB master 14) there; the loader's EMC raise reports
`LOADER_EMC_NOT_T210B01` and leaves the clock where hekate's Minerva put it, and the kernel's TegraBPMP finds no
mailbox and says so.

Still OLED/T210B01-specific, in the order they will stop an Erista boot:

| Where | What |
|---|---|
| `//bazel/pinned:firmware/oled-sku3.dtb`, U-Boot opcode fingerprints and the fixed framebuffer in the boot script | switchroot's Icosa DTB and U-Boot build; LCD framebuffer handoff (`boot/handoff/probe.c`) |
| `drivers/TegraBPMP`, `SwitchPower/SwitchPMIC.cpp` | EMC rate changes go through the B01 BPMP firmware; on a T210 the rate stays at hekate's (the loader and the kernel both skip the raise), the EMC DVFS governor has nothing to drive |
| `NativeMobileDisplay/TegraDisplayPower` | the SC7 cold display init now follows hekate's T210 branches (PLLD SDM/MISC1, DSI init-seq index, no B01 pad trimmers, T210 pad/MIPI calibration) and panel branches (JDI level-2 table, InnoLux/AUO power control, 5.5" settle, AVDD and WLED pins). Written from hekate's di.c/di.inl, never run on a T210; the LCDs' per-panel power-down register tables (`_di_dsi_panel_deinit_config_*`) are not sent before sleep in |
| `drivers/SwitchUSB` pad setup | the falcon firmware is now picked by chip (`tegra21x_xusb_firmware` on a T210); the pad controller sequence (`VREG_FIX18`, the B01 UPHY lane register file in `XusbLanes.h`, no SS clock scaling in the mailbox) is still the B01's and untested on a T210 |
| `drivers/SwitchPower/SwitchThermal.cpp` PLL ladder | on a T210 SwitchThermal now raises the CPU on the DFLL only (MAX77621 rail writes keep bit 7, no M4_CFG/ES2 steps, the T210 DFLL trips 15/66/86 C); its PLL ladder keeps the T210B01's CVB coefficients, so with `switch-no-dfll`, or if the DFLL fails, a T210 stays at hekate's clock |
| `drivers/SwitchPower/SwitchSensors.cpp`, `kits/SwitchIdle` | B01 SOC_THERM sensor set and C7 latencies |
| `drivers/TegraNVDEC`, `TegraGM20BMetal/precompile` | B01 DVFS point; shader cache keyed on chipset 0x12e (Erista reports 0x12b, which `gpu.c` already accepts) |

Same parts on both consoles, expected to carry over: RT5639 audio, BCM4356 Wi-Fi/BT, FTM4 touch, Joy-Con rails,
IMU, MAX77620 PMIC/RTC, MAX17050, BQ24193.

Not Erista-specific: the image builder needs macOS APFS tools.

## Building without a Mac

Upstream's workflows run on GitHub's hosted `xcode-27` image, so a push to `erista` runs CI and a tag equal to
`MODULE.bazel`'s version (`0.1.0-erista.1`) publishes a release of this fork; `install/release.bzl` points at it.
On a Mac, `bazelisk run //:image -- --//install:prebuilt=release ...` then needs no Xcode. An Intel Mac gets
`host-tools-darwin-x86_64.tar` (universal `apfs_tool` and `lz4`, macOS 12 up) but no shader toolset, so the shader
seed still has to come from an Apple silicon Mac.
