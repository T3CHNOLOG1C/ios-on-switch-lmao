/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_SMP_H
#define SWITCH_SMP_H
/* Shared PSCI physical-entry ABI. Four homogeneous Switch Cortex-A57 cores.
 * The boot CPU owns control[0..63]; each secondary exclusively owns status[64..127].
 * Before CPU_ON publish both lines to PoC. After CPU_ON the boot CPU must never
 * dirty the status line; poll it with invalidate + acquire, not a cached load. */
#define IOSW_SMP_CORE_COUNT 4
#define IOSW_SMP_RECORD_BYTES 128
#define IOSW_SMP_STATUS_OFFSET 64
#define IOSW_SMP_MAGIC 0x495357534d503031
#define IOSW_SMP_STAGE_CLEAR 0
#define IOSW_SMP_STAGE_ENTRY 1
#define IOSW_SMP_STAGE_EL1 2
#define IOSW_SMP_STAGE_RESET 3
#define IOSW_SMP_STAGE_MMU 4
#define IOSW_SMP_STAGE_C 5
#define IOSW_SMP_STAGE_FAILED 255
#define IOSW_SMP_ERROR_CONTROL 1
#define IOSW_SMP_ERROR_EL 2
#define IOSW_SMP_ERROR_MPIDR 3
#define IOSW_SMP_ERROR_SCTLR 4
#define IOSW_SMP_ERROR_MIDR 5
#define IOSW_SMP_ERROR_SMPEN 6
#define IOSW_SMP_ERROR_FREQUENCY 7
#define IOSW_SMP_ERROR_CNTVOFF 8
#define IOSW_SMP_ERROR_RESET 9
#define IOSW_SMP_ERROR_EXCEPTION 256
#define IOSW_SMP_MAGIC_OFF 0
#define IOSW_SMP_MPIDR_OFF 8
#define IOSW_SMP_LOGICAL_OFF 16
#define IOSW_SMP_CNTFRQ_OFF 24
#define IOSW_SMP_CNTVOFF_OFF 32
#define IOSW_SMP_RESET_OFF 40
#define IOSW_SMP_RESERVED0_OFF 48
#define IOSW_SMP_RESERVED1_OFF 56
#define IOSW_SMP_STAGE_OFF 64
#define IOSW_SMP_ERROR_OFF 72
#define IOSW_SMP_SEEN_MPIDR_OFF 80
#define IOSW_SMP_ENTRY_EL_OFF 88
#define IOSW_SMP_ESR_OFF 96
#define IOSW_SMP_ELR_OFF 104
#define IOSW_SMP_FAR_OFF 112
#define IOSW_SMP_SCTLR_OFF 120
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
struct switch_smp_record {
    uint64_t magic, mpidr, logical, cntfrq, cntvoff, reset_pa, reserved0, reserved1;
    volatile uint64_t stage, error, seen_mpidr, entry_el, esr, elr, far, sctlr;
} __attribute__((aligned(64)));
#ifdef __cplusplus
static_assert(sizeof(switch_smp_record) == IOSW_SMP_RECORD_BYTES, "SMP record stride");
static_assert(offsetof(switch_smp_record, stage) == IOSW_SMP_STATUS_OFFSET, "SMP status line");
extern "C" {
#else
_Static_assert(sizeof(struct switch_smp_record) == IOSW_SMP_RECORD_BYTES, "SMP record stride");
_Static_assert(offsetof(struct switch_smp_record, stage) == IOSW_SMP_STATUS_OFFSET, "SMP status line");
#endif
extern struct switch_smp_record switch_secondary_records[IOSW_SMP_CORE_COUNT];
extern char switch_secondary_records_end[];
extern char switch_secondary_entry[];
/* Called only after machine_set_current_thread on this secondary. Installs the
 * normal XNU KVA vectors and publishes the C-arrival stage. */
void switch_secondary_vectors_leave(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
