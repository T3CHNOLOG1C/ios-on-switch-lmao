// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraBPMP: the CPU side of the T210 BPMP mailbox (L4T's drivers/firmware/tegra mail.c / mail_t210.c / mailman.c,
// vendored under linux/ with an XNU shim), and the one thing IOSwitch asks the BPMP firmware for: the EMC (DRAM)
// clock, which on T210B01 belongs to the BPMP-FW that hekate starts (TegraBPMP.c says how and why).
#ifndef TEGRA_BPMP_PUBLIC_H
#define TEGRA_BPMP_PUBLIC_H
#ifdef __cplusplus
extern "C" {
#endif

// Brings the mailbox up (once), then sets the EMC clock to `khz` exactly as L4T's clk_set_rate(emc, ...) does on
// T210B01: MRQ_CLK on BPMP clock 0. Thread context; sleeps on BPMP answers (interrupt-driven) and on I2C5. Fills
// `line` with one console line (before -> after, firmware tag, the BPMP's EMC table, the core-voltage check) and
// returns 0, or a negative errno with `line` saying why the EMC clock was left as it was. A BPMP answer whose
// doorbell interrupt never arrives panics. Call with khz 0 to bring the mailbox up and only report.
int tegra_bpmp_emc_boot(unsigned int khz, char *line, unsigned long size);

#ifdef __cplusplus
}
#endif
#endif
