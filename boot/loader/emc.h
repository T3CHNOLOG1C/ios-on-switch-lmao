#ifndef SWITCH_LOADER_EMC_H
#define SWITCH_LOADER_EMC_H
#include <stdint.h>

/* The EMC (DRAM) clock raised before the kernel window is copied, the way the kernel's TegraBPMP.c raises it at
 * SwitchPMIC start (switch-emc=1): hekate leaves DRAM at 204 MHz, so until then the loader's copy, XNU's early VM
 * (pmap_startup) and IOKit start all run at an eighth of the bandwidth. Same requests (MRQ_EMC_DVFS_TABLE, then
 * MRQ_CLK on BPMP clock 0: GET_ALL_INFO "emc", GET_RATE, ENABLE, ROUND_RATE, SET_RATE, GET_RATE), same VDD_CORE gate
 * (MAX77620 SD0 over I2C5 at least L4T's emcb01_dvb_dvfs_table voltage), over L4T's T210 atomic channel (mail.c
 * bpmp_send_receive_atomic on per-CPU channel 0, mail_t210.c): request written to the channel's shared area, slave
 * signalled through the arbitration semaphore, outbound doorbell rung, ACK polled.
 *
 * Polled, not interrupt-driven: nothing in the loader takes interrupts (DAIF masked from entry, no GIC set up) and
 * the BPMP is asked not to ring a CPU doorbell (DO_ACK only), so the semaphore is the only completion there is. Every
 * wait is bounded on CNTPCT; anything unexpected leaves the clock as it is and the boot goes on (result below, in the
 * boot timing record). The kernel later finds EMC at the rate and changes nothing (TegraBPMP.c skips SET_RATE). */
enum loader_emc_result {
    LOADER_EMC_RAISED = 0,     /* now at the rate (or already was) */
    LOADER_EMC_NO_FIRMWARE,    /* arbitration semaphore reads 0: no BPMP firmware handshake */
    LOADER_EMC_CHANNEL,        /* channel 0's shared area is not where BPMP firmware keeps it (IRAM, top of RAM) */
    LOADER_EMC_BUSY,           /* channel 0 never free */
    LOADER_EMC_NO_ANSWER,      /* no ACK in time */
    LOADER_EMC_PING,           /* MRQ_PING answered wrongly */
    LOADER_EMC_TABLE,          /* the rate is not in the firmware's EMC DVFS table */
    LOADER_EMC_NOT_EMC,        /* BPMP clock 0 is not "emc", or GET_RATE failed */
    LOADER_EMC_I2C,            /* MAX77620 SD0 could not be read */
    LOADER_EMC_LOW_CORE,       /* VDD_CORE below what L4T requires for the rate */
    LOADER_EMC_REFUSED,        /* the BPMP refused ENABLE / ROUND_RATE / SET_RATE */
};

struct loader_emc_report {
    uint32_t result;      /* enum loader_emc_result */
    int32_t bpmp_code;    /* the failing request's BPMP return code, or 0 */
    uint32_t before_khz;  /* BPMP clock 0 before, kHz (0 if not read) */
    uint32_t after_khz;   /* and after */
    uint32_t core_mv;     /* VDD_CORE read from SD0, mV (0 if not read) */
    uint32_t area;        /* channel 0's shared area as the BPMP reported it */
};

void loader_emc_raise(uint32_t khz, uint32_t frequency, struct loader_emc_report *report);
#endif
