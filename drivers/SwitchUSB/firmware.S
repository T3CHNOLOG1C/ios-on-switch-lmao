// The XUSB host controller's falcon firmware embedded in the kernel image, one image per Tegra X1 revision
// (firmware/README.md): tegra210b01_xusb_firmware for the T210B01, tegra21x_xusb_firmware for the T210. SwitchXHCI.h
// picks one by the chip. Assemble with -I <this directory>/firmware so .incbin finds them, as SwitchWLAN's firmware.S.
// Read-only data: XusbHost.cpp copies the image into a DMA buffer the falcon loads from.
    .section __DATA_CONST,__const
    .p2align 8
    .globl _switch_xusb_firmware, _switch_xusb_firmware_end
_switch_xusb_firmware: .incbin "tegra210b01_xusb_firmware"
_switch_xusb_firmware_end:
    .p2align 8
    .globl _switch_xusb_firmware_t210, _switch_xusb_firmware_t210_end
_switch_xusb_firmware_t210: .incbin "tegra21x_xusb_firmware"
_switch_xusb_firmware_t210_end:
