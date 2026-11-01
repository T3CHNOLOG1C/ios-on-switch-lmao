// SPDX-License-Identifier: GPL-2.0-only
// Standalone Hekate BPMP installer for an already prepared APFS container.
#include <string.h>
#include <stdlib.h>
#include <bdk.h>
#include "install.h"
#include "storage.h"

#define MAX_PARTS 128
#define MAX_IMAGES 32
static gpt_header_t primary, backup;
static gpt_entry_t *entries, *backup_entries;
static unsigned selected[MAX_PARTS], selected_count, target_index;
static uint32_t table_sectors, table_bytes;
static uint64_t source_first,source_sectors;
static int phase; // 0 read only, 1 selected image extent, 2 exact GPT metadata
static char image_paths[MAX_IMAGES][240];
static unsigned image_count;
static struct iosw_install_image *image;
static uint8_t *source_buffer,*readback_buffer;
static FIL source_file;
static DWORD *source_cltbl;
static int source_open,source_part=-1;
static uint32_t last_progress;
extern void pivot_stack(u32 stack_top);

static int zero(const void *data, unsigned bytes) {
    const u8 *p=data;
    while(bytes--) if(*p++) return 0;
    return 1;
}
static int header_ok(gpt_header_t *h,u64 location) {
    if(memcmp(&h->signature,"EFI PART",8) || h->revision!=0x10000 || h->size<92 || h->size>512 ||
       h->res1 || h->my_lba!=location || h->alt_lba!=(location==1 ? sd_storage.sec_cnt-1 : 1) ||
       h->first_use_lba<2 || h->first_use_lba>h->last_use_lba || h->last_use_lba>=sd_storage.sec_cnt-1 ||
       !h->num_part_ents || h->num_part_ents>MAX_PARTS || h->part_ent_size!=sizeof(gpt_entry_t) ||
       zero(h->disk_guid,16)) return 0;
    u32 crc=h->crc32;h->crc32=0;
    u32 actual=crc32_calc(0,(const u8 *)h,h->size);h->crc32=crc;
    u64 sectors=(h->num_part_ents*sizeof(gpt_entry_t)+511)/512;
    if(crc!=actual || h->part_ent_lba<2 || h->part_ent_lba>=sd_storage.sec_cnt ||
       sectors>sd_storage.sec_cnt-h->part_ent_lba) return 0;
    return location==1 ? h->part_ent_lba+sectors<=h->first_use_lba :
        h->part_ent_lba>h->last_use_lba && h->part_ent_lba+sectors<=location;
}
static int name_is(const u16 *name,const char *expected) {
    unsigned i=0;
    for(;i<36 && expected[i];i++) if(name[i]!=(u8)expected[i]) return 0;
    return i<36 && !expected[i] && name[i]==0;
}
static void name_ascii(char out[37],const u16 *name) {
    unsigned i=0;
    for(;i<36 && name[i];i++) out[i]=name[i]>=32 && name[i]<127 ? name[i] : '?';
    out[i]=0;
}
static const uint8_t apfs_type[16]={0xef,0x57,0x34,0x7c,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
static int load_partitions(void) {
    if(sdmmc_storage_read(&sd_storage,1,1,&primary) || !header_ok(&primary,1) ||
       sdmmc_storage_read(&sd_storage,sd_storage.sec_cnt-1,1,&backup) ||
       !header_ok(&backup,sd_storage.sec_cnt-1) ||
       primary.first_use_lba!=backup.first_use_lba || primary.last_use_lba!=backup.last_use_lba ||
       primary.num_part_ents!=backup.num_part_ents || primary.part_ents_crc32!=backup.part_ents_crc32 ||
       memcmp(primary.disk_guid,backup.disk_guid,16)) return 0;
    table_bytes=primary.num_part_ents*sizeof(gpt_entry_t);table_sectors=(table_bytes+511)/512;
    entries=malloc(table_sectors*512);backup_entries=malloc(table_sectors*512);
    if(!entries || !backup_entries) return 0;
    if(sdmmc_storage_read(&sd_storage,primary.part_ent_lba,table_sectors,entries) ||
       sdmmc_storage_read(&sd_storage,backup.part_ent_lba,table_sectors,backup_entries) ||
       crc32_calc(0,(const u8 *)entries,table_bytes)!=primary.part_ents_crc32 ||
       memcmp(entries,backup_entries,table_bytes)) return 0;
    unsigned source_count=0,apfs_count=0;
    for(unsigned i=0;i<primary.num_part_ents;i++) {
        const gpt_entry_t *p=&entries[i];
        if(zero(p->type_guid,16)) continue;
        if(zero(p->part_guid,16) || p->lba_start<primary.first_use_lba || p->lba_end>primary.last_use_lba ||
           p->lba_start>p->lba_end) return 0;
        for(unsigned j=0;j<i;j++) {
            const gpt_entry_t *q=&entries[j];
            if(!zero(q->type_guid,16) && (!memcmp(p->part_guid,q->part_guid,16) ||
               (p->lba_start<=q->lba_end && q->lba_start<=p->lba_end))) return 0;
        }
        if(!memcmp(p->type_guid,apfs_type,16)) apfs_count++;
        if(p->lba_start==sd_fs.volbase) {source_first=p->lba_start;source_sectors=p->lba_end-p->lba_start+1;source_count++;}
        if((name_is(p->name,"userdata") || name_is(p->name,"UDA")) &&
           !(p->lba_start<=sd_fs.volbase && sd_fs.volbase<=p->lba_end) && !(p->attrs&UINT64_C(0x1000000000000000)))
            selected[selected_count++]=i;
    }
    // One source GPT partition, no existing APFS container that would become ambiguous.
    return source_count==1 && !apfs_count;
}
static void screen(void) {
    gfx_clear_color(0xff000000);gfx_con_setpos(24,24);gfx_puts("IOSwitch installer\n\n");
}
static void image_label(char out[41], const char *name) {
    unsigned n=strlen(name);
    if(n>40) {memcpy(out,name,37);memcpy(out+37,"...",4);}
    else memcpy(out,name,n+1);
}
static u8 key(void) {
    while(btn_read() & (BTN_POWER|BTN_VOL_UP|BTN_VOL_DOWN)) msleep(10);
    return btn_wait();
}
static int unchanged(void) {
    if(installer_card_ready(0) || installer_card_transfer(0,0,1,1,readback_buffer) ||
       memcmp(&primary,readback_buffer,512) ||
       installer_card_transfer(0,0,backup.my_lba,1,readback_buffer) || memcmp(&backup,readback_buffer,512) ||
       installer_card_transfer(0,0,primary.part_ent_lba,table_sectors,readback_buffer) ||
       memcmp(entries,readback_buffer,table_sectors*512) ||
       installer_card_transfer(0,0,backup.part_ent_lba,table_sectors,readback_buffer) ||
       memcmp(backup_entries,readback_buffer,table_sectors*512)) return 1;
    return 0;
}
static uint32_t le16(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
static int source_volume_ok(void) {
    if(installer_card_transfer(0,0,source_first,1,readback_buffer)) return 0;
    uint32_t sectors=le16(readback_buffer+19);
    if(!sectors) sectors=le32(readback_buffer+32);
    return le16(readback_buffer+11)==512 && sectors && sectors<=source_sectors &&
        readback_buffer[510]==0x55 && readback_buffer[511]==0xaa;
}
static int permit(uint64_t sector,uint32_t count) {
    if(!count) return 0;
    if(phase==1) {
        const gpt_entry_t *p=&entries[target_index];
        return sector>=p->lba_start && sector-p->lba_start<image->bytes/512 &&
            count<=image->bytes/512-(sector-p->lba_start);
    }
    if(phase==2) return (sector==1 && count==1) || (sector==backup.my_lba && count==1) ||
        (sector==primary.part_ent_lba && count==table_sectors) ||
        (sector==backup.part_ent_lba && count==table_sectors);
    return 0;
}
static void close_source(void) {
    if(source_open) f_close(&source_file);
    free(source_cltbl);source_cltbl=0;
    source_open=0;source_part=-1;
}
static int source_read(void *unused,uint64_t offset,void *out,uint32_t bytes) {
    (void)unused;
    if(offset>image->source_bytes || bytes>image->source_bytes-offset) return 1;
    uint8_t *p=out;
    while(bytes) {
        uint64_t start=0;unsigned index=0;
        for(;index<image->count;index++) {
            if(offset-start<image->parts[index].bytes) break;
            start+=image->parts[index].bytes;
        }
        if(index==image->count || installer_card_ready(0)) return 1;
        const struct iosw_install_part *part=&image->parts[index];
        if(source_part!=(int)index) {
            close_source();
            if(f_open(&source_file,part->path,FA_READ)!=FR_OK) return 1;
            source_open=1;source_part=index;
            uint64_t cluster_bytes=(uint64_t)source_file.obj.fs->csize*512;
            if(!cluster_bytes || (cluster_bytes&(cluster_bytes-1))) return 1;
            uint64_t entries=2+2*((part->bytes+cluster_bytes-1)/cluster_bytes);
            if(entries>1048576/sizeof(DWORD)) return 1;
            source_cltbl=malloc((size_t)entries*sizeof(DWORD));
            if(!source_cltbl) return 1;
            source_cltbl[0]=(DWORD)entries;
            source_file.cltbl=source_cltbl;
            if(f_lseek(&source_file,CREATE_LINKMAP)!=FR_OK ||
               source_cltbl[0]>entries || f_lseek(&source_file,0)!=FR_OK) return 1;
        }
        if((uint64_t)f_size(&source_file)!=part->bytes ||
           f_lseek(&source_file,(FSIZE_t)(offset-start))!=FR_OK) return 1;
        uint32_t n=part->bytes-(offset-start)<bytes?(uint32_t)(part->bytes-(offset-start)):bytes;
        uint32_t cluster_bytes=source_file.obj.fs->csize*512;
        uint32_t left=n;uint8_t *dest=p;
        while(left) {
            FSIZE_t before=f_tell(&source_file);
            uint32_t within=(uint32_t)(before%cluster_bytes),take;
            if(!within && left>=cluster_bytes) {
                take=left/cluster_bytes*cluster_bytes;
                if(f_read_fast(&source_file,dest,take)!=FR_OK) return 1;
            } else {
                take=cluster_bytes-within;if(take>left)take=left;
                UINT got=0;
                if(f_read(&source_file,dest,take,&got)!=FR_OK || got!=take) return 1;
            }
            if(f_tell(&source_file)!=before+take)return 1;
            dest+=take;left-=take;
        }
        if(installer_card_ready(0)) return 1;
        offset+=n;p+=n;bytes-=n;
    }
    return 0;
}
static void progress(void *unused,uint64_t done,uint64_t total,int writing) {
    (void)unused;(void)writing;
    uint32_t now=get_tmr_ms();
    if(done && done!=total && (uint32_t)(now-last_progress)<250) return;
    last_progress=now;gfx_con_setpos(24,160);
    gfx_printf("Writing (DMA)          \n  %d / %d MiB        \n",(u32)(done>>20),(u32)(total>>20));
}
static int read_manifest(const char *path) {
    FIL file;UINT got;int error=1;
    if(f_open(&file,path,FA_READ)!=FR_OK) return 1;
    FSIZE_t bytes=f_size(&file);
    if(bytes>=64 && bytes<=IOSW_INSTALL_MAX_MANIFEST_BYTES && f_read(&file,source_buffer,bytes,&got)==FR_OK && got==bytes)
        error=iosw_install_manifest(source_buffer,bytes,image);
    f_close(&file);
    if(error || installer_card_ready(0)) return 1;
    for(unsigned i=0;i<image->count;i++) {
        if(f_open(&file,image->parts[i].path,FA_READ)!=FR_OK) return 1;
        int mismatch=(uint64_t)f_size(&file)!=image->parts[i].bytes;f_close(&file);
        if(mismatch || installer_card_ready(0)) return 1;
    }
    return source_read(0,0,source_buffer,image->block_size) ||
        iosw_install_nxsb(image,source_buffer,image->block_size);
}
static int image_menu(void) {
    DIR dir;FILINFO info;
    if(f_opendir(&dir,"IOSWITCH/INSTALL")!=FR_OK) return 0;
    for(;;) {
        if(f_readdir(&dir,&info)!=FR_OK) {f_closedir(&dir);return 0;}
        if(!info.fname[0]) break;
        unsigned n=strlen(info.fname);
        if(!(info.fattrib&AM_DIR) && info.fname[0]!='.' && n>4 && n<220 && !strcmp(info.fname+n-4,".IWI")) {
            if(image_count==MAX_IMAGES) {f_closedir(&dir);return 0;}
            strcpy(image_paths[image_count],"IOSWITCH/INSTALL/");strcat(image_paths[image_count++],info.fname);
        }
    }
    f_closedir(&dir);
    if(!image_count) return 0;
    unsigned current=0;
    for(;;) {
        screen();gfx_puts("Select prepared APFS image\n\n");
        for(unsigned i=current/12*12;i<image_count && i<current/12*12+12;i++) {
            char label[41];image_label(label,image_paths[i]+17);
            gfx_printf("%c %s\n",i==current?'>':' ',label);
        }
        gfx_puts("\nVOL +/- selects. POWER opens.\nBoth volume buttons cancel.\n");
        u8 pressed=key();
        if((pressed&(BTN_VOL_UP|BTN_VOL_DOWN))==(BTN_VOL_UP|BTN_VOL_DOWN)) return 0;
        if(pressed&BTN_VOL_UP) current=current?current-1:image_count-1;
        else if(pressed&BTN_VOL_DOWN) current=(current+1)%image_count;
        else if(pressed&BTN_POWER) {
            if(!read_manifest(image_paths[current])) return 1;
            screen();gfx_puts("Invalid image manifest, files\nor APFS geometry.\nNo writes made. POWER returns.\n");key();
            if(installer_card_ready(0)) return 0;
        }
    }
}
static int partition_menu(void) {
    if(!selected_count) return 0;
    unsigned current=0;
    for(;;) {
        screen();gfx_puts("Select Android partition\n\n");
        for(unsigned i=current/12*12;i<selected_count && i<current/12*12+12;i++) {
            const gpt_entry_t *p=&entries[selected[i]];char name[37];name_ascii(name,p->name);
            gfx_printf("%c %s  %d MiB\n",i==current?'>':' ',name,(u32)((p->lba_end-p->lba_start+1)>>11));
        }
        gfx_puts("\nVOL +/- selects. POWER opens.\nBoth volume buttons cancel.\n");
        u8 pressed=key();
        if((pressed&(BTN_VOL_UP|BTN_VOL_DOWN))==(BTN_VOL_UP|BTN_VOL_DOWN)) return 0;
        if(pressed&BTN_VOL_UP) current=current?current-1:selected_count-1;
        else if(pressed&BTN_VOL_DOWN) current=(current+1)%selected_count;
        else if(pressed&BTN_POWER) {
            target_index=selected[current];const gpt_entry_t *p=&entries[target_index];
            if(image->bytes/512<=p->lba_end-p->lba_start+1) return 1;
            screen();gfx_puts("Image is larger than this partition.\nNo writes were made. POWER returns.\n");key();
        }
    }
}
static int confirm(void) {
    const gpt_entry_t *p=&entries[target_index];char name[37];name_ascii(name,p->name);
    screen();gfx_printf("ERASE ANDROID PARTITION: %s\n\n",name);
    gfx_printf("First LBA: 0x%08x%08x\nSectors:   0x%08x%08x\n",(u32)(p->lba_start>>32),(u32)p->lba_start,
        (u32)((p->lba_end-p->lba_start+1)>>32),(u32)(p->lba_end-p->lba_start+1));
    gfx_printf("Partition: %d MiB\nAPFS image: %d MiB\n\n",(u32)((p->lba_end-p->lba_start+1)>>11),(u32)(image->bytes>>20));
    gfx_printf("Data to write: %d MiB\n\n",(u32)((image->copy_bytes+1048575)>>20));
    gfx_puts("ALL ANDROID DATA HERE WILL BE LOST.\n\nKeep power connected.\nDo not remove the card.\nInterrupted GPT updates need recovery.\n\nPress VOL UP to ERASE and start.\nVOL DOWN cancels.\n");
    while(btn_read()&(BTN_POWER|BTN_VOL_UP|BTN_VOL_DOWN)) msleep(10);
    for(;;) {
        if(installer_card_ready(0)) return 0;
        u8 pressed=btn_read();
        if(pressed&BTN_VOL_DOWN) return 0;
        if(pressed&BTN_VOL_UP) return 1;
        msleep(10);
    }
}
static void header_crc(gpt_header_t *h,uint32_t table_crc) {
    h->part_ents_crc32=table_crc;h->crc32=0;h->crc32=crc32_calc(0,(const u8 *)h,h->size);
}
static int write_verify(uint64_t lba,uint32_t sectors,void *bytes) {
    return installer_card_transfer(0,1,lba,sectors,bytes) || installer_card_sync(0) ||
        installer_card_transfer(0,0,lba,sectors,readback_buffer) || memcmp(bytes,readback_buffer,sectors*512);
}
static int publish(void) {
    if(unchanged() || installer_card_qualify()) return 1;
    gpt_header_t new_primary=primary,new_backup=backup;
    gpt_entry_t *a=malloc(table_sectors*512),*b=malloc(table_sectors*512);
    if(!a || !b) {free(a);free(b);return 1;}
    memcpy(a,entries,table_sectors*512);memcpy(b,backup_entries,table_sectors*512);
    memcpy(a[target_index].type_guid,apfs_type,16);memset(a[target_index].name,0,72);
    const char *name="IOSWITCH";for(unsigned i=0;name[i];i++) a[target_index].name[i]=name[i];
    memcpy(&b[target_index],&a[target_index],sizeof(gpt_entry_t));
    uint32_t crc=crc32_calc(0,(const uint8_t *)a,table_bytes);
    header_crc(&new_primary,crc);header_crc(&new_backup,crc);
    phase=2;
    // Publish backup first, primary last. No blind retry/repair after uncertain I/O.
    int error=write_verify(backup.part_ent_lba,table_sectors,b) || write_verify(backup.my_lba,1,&new_backup) ||
        write_verify(primary.part_ent_lba,table_sectors,a) || write_verify(1,1,&new_primary) || installer_card_sync(0);
    phase=0;free(a);free(b);return error;
}
static void run(void) {
    image=malloc(sizeof(*image));source_buffer=malloc(IOSW_INSTALL_CHUNK);readback_buffer=malloc(IOSW_INSTALL_CHUNK);
    if(!image || !source_buffer || !readback_buffer || installer_card_bind() || unchanged() || !source_volume_ok()) {
        gfx_puts("Card identity, source volume\nor memory check failed.\nNo writes were made.\n");return;
    }
    installer_card_permit(permit);
    if(!image_menu() || !partition_menu()) {screen();gfx_puts("Cancelled, or no eligible\nimage/partition. No writes made.\n");return;}
    struct iosw_install_io io={0,installer_card_ready,source_read,installer_card_transfer,installer_card_sync,progress};
    const gpt_entry_t *p=&entries[target_index];
    close_source();
    if(unchanged() || installer_card_qualify()) {
        screen();gfx_puts("Source, card identity or cache\ncheck failed. No writes were made.\n");return;
    }
    if(!confirm()) {screen();gfx_puts("Cancelled. No writes were made.\n");return;}
    if(unchanged() || installer_card_qualify()) {screen();gfx_puts("Card changed. No writes were made.\n");return;}
    phase=1;screen();gfx_puts("Installing APFS container.\nKeep power connected.\n");
    int error=iosw_install_stream(image,&io,p->lba_start,p->lba_end-p->lba_start+1,source_buffer,IOSW_INSTALL_CHUNK);
    phase=0;close_source();
    if(error) {
        installer_card_offline();screen();gfx_puts("INSTALL FAILED.\nTarget contents may be incomplete.\nGPT was not published.\nDo not boot Android here.\n");return;
    }
    screen();gfx_puts("Image written.\nPublishing standard APFS GPT entry...\n");
    if(publish()) {
        installer_card_offline();gfx_puts("\nGPT UPDATE FAILED.\nRecovery may be required.\nDo not boot this partition.\n");return;
    }
    screen();gfx_puts("Installation complete.\nDMA writes and GPT update complete.\n\nStandard APFS partition: IOSWITCH\nPower off, return to Hekate, then select\nIOSwitch Native APFS.\n");
}
void ipl_main(void) {
    hw_init();pivot_stack(IPL_LOAD_ADDR);heap_init((void *)IPL_HEAP_START);
    display_init();u32 *fb=display_init_window_a_pitch();
    bpmp_clk_rate_set(hw_get_chip_id()==GP_HIDREV_MAJOR_T210B01 ? BPMP_CLK_DEFAULT_BOOST : BPMP_CLK_LOWER_BOOST);
    gfx_init_ctxt(fb,720,1280,720);gfx_con_init();gfx_con_setcol(0xffffffff,1,0xff000000);
    display_backlight_pwm_init();display_backlight_brightness(100,1000);screen();
    if(sd_mount()) gfx_puts("SD card could not be mounted.\n");
    else if(!load_partitions()) gfx_puts("GPT/source partition invalid,\nor APFS already exists.\nNo writes were made.\n");
    else run();
    gfx_puts("\nReady to power off. Press POWER.\n");key();
    sd_end();power_set_state(POWER_OFF);for(;;) bpmp_halt();
}
