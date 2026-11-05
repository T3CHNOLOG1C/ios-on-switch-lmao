// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE: the T210B01 Security Engine SE2 for the kernel (TegraSE.cpp). Never booted (r17-crypto).
#ifndef TEGRA_SE_H
#define TEGRA_SE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// From the board thread (IOSwitchRootDiag.inc): starts the driver's own thread and returns. That thread checks the
// engine (registers, interrupt, known answers), reseeds the kernel PRNG from SE2's DRBG, and 90 s later times SE2
// against the A57s' crypto instructions and logs the numbers. Any check that fails is one line and the driver off.
int tegra_se_start(void);

// SE2's one owner at a time: the AppleKeyStore stand-in's boot probe (NativeKeyStoreEvents/TegraSecurityEngine.cpp)
// and this driver. Thread context; waits up to `ms` for the other; false if it did not get the engine.
bool tegra_se2_claim(uint32_t owner, uint32_t ms);
void tegra_se2_release(uint32_t owner);
#define TEGRA_SE2_OWNER_AKS_PROBE 1u
#define TEGRA_SE2_OWNER_DRIVER    2u

#ifdef __cplusplus
}
#endif
#endif
