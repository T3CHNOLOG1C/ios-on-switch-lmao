/* Installer uses upstream contiguous-cluster DMA reads. */
#include "../../out/research-native/sol-hekate-source/bootloader/libs/fatfs/ffconf.h"
#undef FF_FASTFS
#define FF_FASTFS 1
#undef FF_USE_FASTSEEK
#define FF_USE_FASTSEEK 1
