# tegra210b01_xusb_firmware

The Tegra X1 (T210B01, "Mariko") XUSB host controller's falcon firmware, loaded by `XusbHost.cpp`
(`Controller::loadFirmware`, after Linux `tegra_xhci_load_firmware`) and embedded by `../firmware.S`.

- Source: https://github.com/TheMuppets/proprietary_vendor_nvidia, branch `lineage-19.1`,
  path `t210/firmware/xusb/tegra210b01_xusb_firmware` (the LineageOS NVIDIA vendor tree that switchroot's Android
  builds use; it is not in any gitlab.com/switchroot repository: nx-firmware, proprietary_vendor_* and l4t-* were
  searched).
- Git blob: `7fb94bbb9063303cdf797a6f73e8ccc5e548befe`
- SHA-256: `de36635ad5680fdcd483dde9fe10b9bdf54633325bdc17bc7ce011e0742be6d3`
- Size: 122368 bytes; header magic `XUSBFW` at offset 0x68.
- Licence: NVIDIA proprietary firmware, redistributed in binary form by NVIDIA's Tegra BSPs and the vendor trees above;
  no separate licence file accompanies it there. Linux loads the same file by this name (`tegra210b01_soc.firmware`
  in switch-l4t-kernel-4.9 drivers/usb/host/xhci-tegra.c).
