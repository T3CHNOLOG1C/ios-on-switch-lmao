// The XUSB host controller's falcon firmware (tegra210b01_xusb_firmware, see firmware/README.md) embedded in the
// kernel image. Assemble with -I <this directory>/firmware so .incbin finds it, as SwitchWLAN's firmware.S. Read-only
// data: XusbHost.cpp copies it into a DMA buffer the falcon loads from.
    .section __DATA_CONST,__const
    .p2align 8
    .globl _switch_xusb_firmware, _switch_xusb_firmware_end
_switch_xusb_firmware: .incbin "tegra210b01_xusb_firmware"
_switch_xusb_firmware_end:
