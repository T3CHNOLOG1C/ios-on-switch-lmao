// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC falcon firmware embedded in the kernel image (firmware/SOURCE). Assemble with -I <this directory>/firmware.
    .section __DATA_CONST,__const
    .globl _tegra_nvdec_fw_start
    .globl _tegra_nvdec_fw_end
    .p2align 8
_tegra_nvdec_fw_start:
    .incbin "nvidia/tegra210/nvdec.bin"
_tegra_nvdec_fw_end:
