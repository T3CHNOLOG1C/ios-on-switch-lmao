/*
 * Copyright (c) 2016 Apple Inc. All rights reserved.
 */

#ifndef _PEXPERT_ARM_BCM2837_H
#define _PEXPERT_ARM_BCM2837_H

#define NO_MONITOR 1
#define NO_ECORE 1

#define BCM2837
#define ARM_ARCH_TIMER

#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM64_PMAP_SUBPAGE_L1__ 1

/* Explicit native ARMv8.1+ bring-up; the original A53 target has no PAN. */
#if defined(XNU_NATIVE_PAN) && XNU_NATIVE_PAN
#define __ARM_PAN_AVAILABLE__      1
#endif

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
/* Apple CPMU registers are implementation defined; counters stay off and
 * this only sizes the per-CPU counter state. */
#define CPMU_PMC_COUNT 10
#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_BCM2837_H */
