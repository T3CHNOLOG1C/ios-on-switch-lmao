// BCM4356A3 Wi-Fi firmware and board NVRAM embedded in the kernel image (files under firmware/, see
// firmware/SOURCE and firmware/WHENCE). Assemble with -I <this directory>/firmware so .incbin finds them,
// as TegraGM20B's firmware.S. Read-only data: the driver copies the image into the chip's RAM over PCIe and
// converts the NVRAM text into the chip's format first, so neither is ever executed or written here.
    .section __DATA_CONST,__const
    .p2align 3
    .globl _switch_wlan_fw_image, _switch_wlan_fw_image_size, _switch_wlan_fw_nvram, _switch_wlan_fw_nvram_size
_switch_wlan_fw_image_size: .quad Limage_end - _switch_wlan_fw_image
_switch_wlan_fw_nvram_size: .quad Lnvram_end - _switch_wlan_fw_nvram
    .p2align 6
_switch_wlan_fw_image: .incbin "brcm/brcmfmac4356A3-pcie.bin"
Limage_end:
    .p2align 3
_switch_wlan_fw_nvram: .incbin "brcm/brcmfmac4356A3-pcie.txt"
Lnvram_end:
    .byte 0
