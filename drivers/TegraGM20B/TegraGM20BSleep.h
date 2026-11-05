// SPDX-License-Identifier: MIT
// The GM20B across system sleep (SC7), for drivers/SwitchSuspend. Both run on SwitchSuspend's sleep/wake thread with
// interrupts on and I2C up: suspend after the display's, resume after host1x's and before the display's (its GPU present
// needs the GPU). Neither returns before the GPU is fully off or fully back; a GPU that cannot get there panics with one
// line. Each step is one power-journal line ("tegra-gm20b: sleep ..." / "tegra-gm20b: wake ..."). Both are no-ops when
// tegra-gpu is off or the GPU was never powered. Submissions and new channels made meanwhile wait for the resume.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void tegra_gm20b_suspend(void);
void tegra_gm20b_resume(void);

#ifdef __cplusplus
}
#endif
