// SPDX-License-Identifier: GPL-2.0-only
// hekate's FatFs configuration with fast reads: runs of contiguous clusters in one command (f_read_fast).
#include "libs/fatfs/ffconf.h"

#undef FF_FASTFS
#define FF_FASTFS 1
#undef FF_USE_FASTSEEK
#define FF_USE_FASTSEEK 1
