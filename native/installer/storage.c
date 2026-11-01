// SPDX-License-Identifier: GPL-2.0-only
// Official Hekate DMA command path, deliberately without retry/reinitialization.
#include <bdk.h>
#include <storage/mmc_def.h>
#include <storage/sd_def.h>
#include <string.h>
#include <stdlib.h>
#include "install.h"
#include "storage.h"
static uint8_t cid[16], *bounce;
static uint32_t capacity, rca;
static int bound, offline, qualified;
static int (*write_permit)(uint64_t, uint32_t);
/* Include locked, underrun/overrun and authentication errors as well as BDK's
 * ordinary R1 error mask. Unknown/error state never authorizes a write. */
#define CARD_ERRORS UINT32_C(0xffffe088)
void installer_card_offline(void) { offline=1;qualified=0;write_permit=0; }
static int fail(void) { installer_card_offline();return 1; }
int installer_card_ready(void *unused) {
    (void)unused;
    if(!bound || offline || !sd_get_card_initialized() || !sdmmc_get_sd_inserted() ||
       sd_get_card_removed() || !sd_storage.initialized || sd_storage.sdmmc!=&sd_sdmmc || sd_sdmmc.id!=SDMMC_1 ||
       !sd_storage.has_sector_access || sd_storage.sec_cnt!=capacity || sd_storage.rca!=rca ||
       memcmp(cid,sd_storage.raw_cid,16)) return fail();
    return 0;
}
int installer_card_bind(void) {
    if(bound || !sd_storage.initialized || !sd_storage.has_sector_access ||
       sd_storage.sdmmc!=&sd_sdmmc || sd_sdmmc.id!=SDMMC_1 || !sd_storage.sec_cnt || !sd_storage.rca) return 1;
    bounce=malloc(IOSW_INSTALL_CHUNK);
    if(!bounce || ((uintptr_t)bounce&31) || !mc_client_has_access(bounce) ||
       !mc_client_has_access(bounce+IOSW_INSTALL_CHUNK-1)) return 1;
    memcpy(cid,sd_storage.raw_cid,16);capacity=sd_storage.sec_cnt;rca=sd_storage.rca;bound=1;
    return installer_card_ready(0);
}
void installer_card_permit(int (*permit)(uint64_t, uint32_t)) { write_permit=permit; }
static int command(uint32_t index, uint32_t arg, void *bytes, uint32_t size, int write, int multi) {
    if(installer_card_ready(0)) return 1;
    sdmmc_cmd_t cmd;sdmmc_req_t req;u32 count=0,response;
    sdmmc_init_cmd(&cmd,index,arg,SDMMC_RSP_TYPE_1,0);
    if(size) {
        if(size>IOSW_INSTALL_CHUNK) return fail();
        if(write) memcpy(bounce,bytes,size);
        /* Installer-owned aligned DRAM can receive DMA directly. Small/stack
         * buffers retain the qualified private bounce buffer. Writes always
         * stage the complete request before issuing the command. */
        uintptr_t address=(uintptr_t)bytes;
        int direct=!write && !(address&31) && address<=UINTPTR_MAX-(size-1) &&
            mc_client_has_access(bytes) && mc_client_has_access((uint8_t *)bytes+size-1);
        req.buf=direct?bytes:bounce;req.blksize=multi?512:size;req.num_sectors=multi?size/512:1;
        req.is_write=write;req.is_multi_block=multi;req.is_auto_stop_trn=multi;
    }
    if(sdmmc_execute_cmd(&sd_sdmmc,&cmd,size?&req:0,size?&count:0) ||
       sdmmc_get_cached_rsp(&sd_sdmmc,&response,SDMMC_RSP_TYPE_1) ||
       (response&CARD_ERRORS) || (multi && (sd_sdmmc.stop_trn_rsp&CARD_ERRORS)) ||
       (size && count!=(multi?size/512:1)) ||
       installer_card_ready(0)) return fail();
    if(size && !write && req.buf!=bytes) memcpy(bytes,bounce,size);
    return 0;
}
int installer_card_sync(void *unused) {
    (void)unused;
    uint32_t started=get_tmr_ms();
    for(;;) {
        if(command(MMC_SEND_STATUS,rca<<16,0,0,0,0)) return 1;
        u32 response;
        if(sdmmc_get_cached_rsp(&sd_sdmmc,&response,SDMMC_RSP_TYPE_1)) return fail();
        uint32_t state=R1_CURRENT_STATE(response);
        if(state==R1_STATE_TRAN && (response&R1_READY_FOR_DATA)) return 0;
        if((state!=R1_STATE_PRG && state!=R1_STATE_TRAN) || (uint32_t)(get_tmr_ms()-started)>=30000)
            return fail();
        msleep(1);
    }
}
int installer_card_transfer(void *unused, int write, uint64_t sector, uint32_t count, void *bytes) {
    (void)unused;
    if(!bytes || !count || count>IOSW_INSTALL_CHUNK/512 || sector>=capacity || count>capacity-sector)
        return fail();
    if(write && (!qualified || !write_permit || !write_permit(sector,count))) return fail();
    if(command(write?MMC_WRITE_MULTIPLE_BLOCK:MMC_READ_MULTIPLE_BLOCK,(uint32_t)sector,
               bytes,count*512,write,1)) return 1;
    return installer_card_sync(0);
}
/* Never allow FatFs or another linked BDK caller to write. */
int __wrap_sdmmc_storage_write(sdmmc_storage_t *s,u32 sector,u32 count,void *buffer) {
    (void)s;(void)sector;(void)count;(void)buffer;return 1;
}
int __real_sdmmc_storage_read(sdmmc_storage_t *,u32,u32,void *);
int __wrap_sdmmc_storage_read(sdmmc_storage_t *s,u32 sector,u32 count,void *buffer) {
    if(!bound) return __real_sdmmc_storage_read(s,sector,count,buffer);
    if(s!=&sd_storage || !count || !buffer) return fail();
    uint8_t *p=buffer;
    while(count) {
        uint32_t n=count>IOSW_INSTALL_CHUNK/512?IOSW_INSTALL_CHUNK/512:count;
        if(installer_card_transfer(0,0,sector,n,p)) return 1;
        sector+=n;count-=n;p+=n*512;
    }
    return 0;
}
static uint32_t le16(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p)|le16(p+2)<<16; }
/* Same conservative SD Physical Layer 6.00 SCR/CMD48 qualification as the
 * native writable backend. Do not enable/disable a cache or guess at layouts. */
int installer_card_qualify(void) {
    uint8_t bytes[512],general[512];u32 response;
    if(installer_card_sync(0) || command(MMC_APP_CMD,rca<<16,0,0,0,0) ||
       sdmmc_get_cached_rsp(&sd_sdmmc,&response,SDMMC_RSP_TYPE_1) || !(response&R1_APP_CMD) ||
       command(SD_APP_SEND_SCR,0,bytes,8,0,0)) return fail();
    uint64_t scr=0;for(unsigned i=0;i<8;i++) scr=scr<<8|bytes[i];
    if(scr>>60 || ((scr>>56)&15)!=2 || ((scr>>48)&5)!=5 ||
       ((scr&(UINT64_C(1)<<35)) && !(scr&(UINT64_C(1)<<34)))) return fail();
    if(scr&(UINT64_C(1)<<34)) {
        if(!(scr&(UINT64_C(1)<<47)) || command(SD_READ_EXTR_SINGLE,0x1ff,general,512,0,0)) return fail();
        uint32_t length=le16(general+2),total=general[4],ext=16,seen[10],perf=0;
        if(le16(general) || length<16 || length>512 || !total || total>10) return fail();
        for(uint32_t i=0;i<total;i++) {
            if(ext<16 || ext>length || length-ext<48) return fail();
            for(uint32_t j=0;j<i;j++) if(ext<seen[j]+48 && seen[j]<ext+48) return fail();
            seen[i]=ext;
            uint32_t next=le16(general+ext+40);
            if(general[ext+42]!=1 || (i+1==total?next!=0:next==0)) return fail();
            if(le16(general+ext)==2) {
                uint32_t address=le32(general+ext+44),fno=(address>>18)&15,page=(address>>9)&255;
                if(perf++ || (address&0xffc201ffu) || !fno ||
                   command(SD_READ_EXTR_SINGLE,fno<<27|page<<18|0x1ff,bytes,512,0,0) ||
                   bytes[0] || (bytes[260]&1) || (bytes[262]&1)) return fail();
            }
            ext=next;
        }
    }
    if(installer_card_sync(0)) return 1;
    qualified=1;return 0;
}
