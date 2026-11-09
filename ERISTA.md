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
| `drivers/SwitchBrightness` (`TegraPanel*`), `NativeMobileDisplay/TegraDisplayPower` | OLED brightness is DCS 0x51 over DSI; Erista LCDs (JDI/InnoLux/AUO) have a PWM backlight and other on/off sequences |
| `drivers/SwitchUSB/firmware` | embeds `tegra210b01_xusb_firmware`; Erista needs `tegra21x_xusb_firmware`; `VREG_FIX18` pad setup is B01's |
| `drivers/TegraDFLL`, `SwitchPower/SwitchPMIC.cpp`, `tools/emu/patches/0031` | VDD_CPU is a MAX77812 (I2C5 0x31/0x33); Erista has MAX77621s (CPU 0x1B, GPU 0x1C), T210 CVB/speedo tables. Without it `SwitchThermal` falls back to the fixed PLLX ladder |
| `drivers/SwitchPower/SwitchSensors.cpp`, `kits/SwitchIdle` | B01 SOC_THERM sensor set and C7 latencies |
| `drivers/TegraNVDEC`, `TegraGM20BMetal/precompile` | B01 DVFS point; shader cache keyed on chipset 0x12e (Erista reports 0x12b, which `gpu.c` already accepts) |

Same parts on both consoles, expected to carry over: RT5639 audio, BCM4356 Wi-Fi/BT, FTM4 touch, Joy-Con rails,
IMU, MAX77620 PMIC/RTC, MAX17050, BQ24193.

Not Erista-specific: the image builder needs macOS APFS tools.
