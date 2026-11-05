// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 memory controller's SMMU registers, shared by every driver that translates a swgroup (TegraVIC, TegraNVJPG;
// the display's direct scanout next). Selecting a directory takes two global registers in turn (PTB_ASID, then PTB_DATA) and
// the PTC/TLB flushes are global too, so two drivers programming at once would program each other's ASID: every access
// goes through here, under one lock. Page tables are TegraSMMUDomain.h.
//
// Registers (Linux memory/tegra/tegra210.c, iommu/tegra-smmu.c): PTB_ASID 0x1c, PTB_DATA 0x20, TLB_FLUSH 0x30 (ASID match
// bit 31, ASID in 30:24 here written as asid << 24), PTC_FLUSH 0x34 (0: all). A swgroup's ASID register is ENABLE 31 |
// ASID; each memory client has a translation bit in TRANSLATION_ENABLE_0..4 (0x228..0x238), which switchroot's bl31 sets
// for every client while leaving every swgroup in bypass. The global SMMU enable is secure-only (reads back garbage), so a
// user proves translation itself (TegraVIC V3).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ASIDs in use, one swgroup each.
enum { TEGRA_SMMU_ASID_VIC = 127, TEGRA_SMMU_ASID_DISPLAY = 126, TEGRA_SMMU_ASID_NVJPG = 125 };
enum {
    TEGRA_SMMU_TRANSLATION_ENABLE_0 = 0x228,
    TEGRA_SMMU_TRANSLATION_ENABLE_3 = 0x234,
    TEGRA_SMMU_DC_ASID = 0x240,
    TEGRA_SMMU_VIC_ASID = 0x284,
    TEGRA_SMMU_NVJPG_ASID = 0xac0,  // swgroup "nvjpg" (clients nvjpgsrd/nvjpgswr: TRANSLATION_ENABLE_3 bits 30/31)
};

// Points swgroup register asidReg at the directory ptb (TegraSMMUDomain::ptb) under asid: the directory (read back),
// a flush, the clients' translation bits in translationReg, the swgroup, a flush. 0, or -1 with one line in `line`: the
// swgroup is translated by another ASID, or a register did not take.
int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg, uint32_t translationBits,
                      char *line, size_t size);
// The swgroup back to bypass, the ASID's directory cleared, a flush.
void tegra_smmu_detach(uint32_t asid, uint32_t asidReg);
// After a change to the ASID's tables (cleaned to memory first): PTC all, then the ASID's TLB entries.
void tegra_smmu_flush(uint32_t asid);
// The MC register at offset (for a driver's own checks and FAIL lines).
uint32_t tegra_smmu_read(uint32_t offset);

#ifdef __cplusplus
}
#endif
