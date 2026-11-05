#ifndef IOSWITCH_FILES_H
#define IOSWITCH_FILES_H
#include <stdint.h>
#include "storage/sdmmc_read.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { IOSW_STORAGE_FAT = 0, IOSW_STORAGE_RAW = 1, IOSW_STORAGE_INVALID = -1 };
/* Parsed once during probe; absent/fat retains the existing file backend.
 * Unknown values are fatal. raw is root-boot-only, never a fallback. */
int ioswitch_files_storage_profile(void);
struct iosw_raw_apfs;
int ioswitch_files_raw_open(struct iosw_raw_apfs *, void *private_scratch, uint32_t);
#define IOSW_FILES_MAGIC UINT64_C(0x494f535746494c45)
/* magic,version,stage,error,FATerror,SDerror,capacity,partitionstart,
 * partitionsize,clusterbytes,sectorsread,lastLBA,SDstatus,SDstage,readymask,
 * blocked,fixturebytes,fixtureverified,volumesopened,rangesverified,
 * rangeindex,volumeindex,rangeoffset,rangelength,verifiedbytes,elapsedticks,
 * frequency,sectorbudget,secondsbudget,SHAkat,totalranges,totalvolumes,
 * GPTerror,partitionindex,BPBsectors,GPTentrycount,GPTentrysize,reserved[3]. */
extern volatile uint64_t ioswitch_files_record[40];
extern uint8_t ioswitch_files_digest[32];
extern uint8_t ioswitch_files_expected_digest[32];
void ioswitch_files_probe(struct iosw_sd_host *);
void ioswitch_diag_files_update(void);
#ifdef __cplusplus
}
#endif
#endif
