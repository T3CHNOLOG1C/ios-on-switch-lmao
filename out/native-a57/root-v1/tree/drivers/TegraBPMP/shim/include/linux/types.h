/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* IOSwitch TegraBPMP shim: Linux scalar types for the vendored L4T BPMP sources. */
#ifndef BPXNU_LINUX_TYPES_H
#define BPXNU_LINUX_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u64 dma_addr_t;
typedef u64 phys_addr_t;
typedef unsigned int gfp_t;
typedef unsigned short umode_t;
typedef unsigned short mode_t;
typedef s64 ktime_t;
#define __iomem
#define __force
#define __init
#define __packed __attribute__((packed))
#endif
