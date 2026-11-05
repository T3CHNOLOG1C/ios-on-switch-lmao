#include "partition.h"

/* UEFI 2.11 section 5.3, including tables 5.5/5.6 and array placement:
 * https://uefi.org/specs/UEFI/2.11/05_GUID_Partition_Table_Format.html
 * This intentionally implements a bounded subset, not a repair utility.
 */
static const uint8_t basic_data[16]={
    0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,
    0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7
};
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p)|((uint64_t)get32(p+4)<<32);
}
static int equal(const uint8_t *a,const uint8_t *b,uint32_t n) {
    for (uint32_t i=0;i<n;i++) if(a[i]!=b[i]) return 0;
    return 1;
}
static int zero(const uint8_t *p,uint32_t n) {
    for (uint32_t i=0;i<n;i++) if(p[i]) return 0;
    return 1;
}
static void copy(uint8_t *to,const uint8_t *from,uint32_t n) {
    for(uint32_t i=0;i<n;i++) to[i]=from[i];
}
static uint32_t crc_update(uint32_t crc,const uint8_t *p,uint32_t n) {
    for (uint32_t i=0;i<n;i++) {
        crc^=p[i];
        for (uint32_t bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u&((uint32_t)0-(crc&1)));
    }
    return crc;
}
static int read_sector(switch_fat_read512 read512,void *context,uint64_t capacity,
                        uint64_t lba,uint8_t *out) {
    if (lba>=capacity) return SWITCH_GPT_GEOMETRY;
    return read512(context,lba,out)?SWITCH_GPT_IO:SWITCH_GPT_OK;
}
static int parse_header(uint8_t *p,uint64_t capacity,uint32_t backup,
                         struct switch_gpt_header *h) {
    static const uint8_t signature[8]={'E','F','I',' ','P','A','R','T'};
    if (!equal(p,signature,8) || get32(p+8)!=0x10000 || get32(p+12)!=92 ||
        get32(p+20) || !zero(p+92,512-92)) return SWITCH_GPT_HEADER;
    uint32_t expected=get32(p+16);
    p[16]=p[17]=p[18]=p[19]=0;
    if ((crc_update(0xffffffffu,p,92)^0xffffffffu)!=expected) return SWITCH_GPT_HEADER_CRC;
    if (get64(p+24)!=(backup?capacity-1:1) ||
        get64(p+32)!=(backup?1:capacity-1)) return SWITCH_GPT_GEOMETRY;
    h->first_usable=get64(p+40); h->last_usable=get64(p+48);
    h->entry_lba=get64(p+72); h->entry_count=get32(p+80);
    h->entry_size=get32(p+84); h->array_crc=get32(p+88);
    copy(h->disk_guid,p+56,16);
    if (zero(h->disk_guid,16) || h->first_usable<34 ||
        h->first_usable>h->last_usable || h->last_usable>=capacity-1)
        return SWITCH_GPT_GEOMETRY;
    if (!h->entry_count || h->entry_count>SWITCH_GPT_MAX_ENTRIES ||
        (h->entry_size!=128 && h->entry_size!=256 && h->entry_size!=512))
        return SWITCH_GPT_ENTRIES;
    uint64_t bytes=(uint64_t)h->entry_count*h->entry_size;
    if (bytes>65536) return SWITCH_GPT_ENTRIES;
    uint64_t sectors=(bytes+511)/512;
    /* Hekate declares only created entries and CRCs their exact bytes, but
     * reserves a full 16KiB array. Keep that reservation bound independent
     * of the smaller declared count; array reads/CRCs still use bytes. */
    if (sectors<32) sectors=32;
    if (!backup) {
        if(h->entry_lba<2 || h->entry_lba>=h->first_usable ||
           sectors>h->first_usable-h->entry_lba) return SWITCH_GPT_GEOMETRY;
    } else {
        if(h->entry_lba<=h->last_usable || h->entry_lba>=capacity-1 ||
           sectors>capacity-1-h->entry_lba) return SWITCH_GPT_GEOMETRY;
    }
    return SWITCH_GPT_OK;
}
static int parse_entry(const uint8_t *p,uint32_t index,
                        const uint8_t expected[16],struct switch_gpt_workspace *w,
                        uint32_t *used,uint32_t *found) {
    const struct switch_gpt_header *h=&w->headers[0];
    if (!zero(p+128,h->entry_size-128)) return SWITCH_GPT_ENTRIES;
    if (zero(p,16)) return SWITCH_GPT_OK;
    uint64_t first=get64(p+32),last=get64(p+40);
    if (zero(p+16,16) || first<h->first_usable || first>last || last>h->last_usable)
        return SWITCH_GPT_ENTRIES;
    for (uint32_t i=0;i<*used;i++) {
        if (equal(p+16,w->used[i].guid,16)) return SWITCH_GPT_DUPLICATE;
        if (first<=w->used[i].last && w->used[i].first<=last) return SWITCH_GPT_OVERLAP;
    }
    struct switch_gpt_extent *extent=&w->used[(*used)++];
    extent->first=first; extent->last=last; copy(extent->guid,p+16,16);
    if (equal(p,basic_data,16) && equal(p+16,expected,16)) {
        if (*found) return SWITCH_GPT_DUPLICATE;
        w->candidate.first_sector=first;
        w->candidate.sector_count=last-first+1;
        w->candidate.partition_index=index;
        copy(w->candidate.unique_guid,p+16,16);
        *found=1;
    }
    return SWITCH_GPT_OK;
}

int switch_gpt_find(switch_fat_read512 read512,void *context,uint64_t capacity,
                  const uint8_t expected[16],struct switch_gpt_workspace *w,
                  struct switch_gpt_slice *output) {
    if(!read512 || !expected || !w || !output || capacity<68 || zero(expected,16))
        return SWITCH_GPT_ARGUMENT;
    int rc=read_sector(read512,context,capacity,0,w->sectors[0]);
    if(rc) return rc;
    if(w->sectors[0][510]!=0x55 || w->sectors[0][511]!=0xaa) return SWITCH_GPT_MBR;
    uint32_t protective=0;
    for(uint32_t i=0;i<4;i++) if(w->sectors[0][446+i*16+4]==0xee) protective=1;
    if(!protective) return SWITCH_GPT_MBR;
    for(uint32_t i=0;i<2;i++) {
        rc=read_sector(read512,context,capacity,i?capacity-1:1,w->sectors[i]);
        if(rc) return rc;
        rc=parse_header(w->sectors[i],capacity,i,&w->headers[i]);
        if(rc) return rc;
    }
    const struct switch_gpt_header *a=&w->headers[0],*b=&w->headers[1];
    if(a->first_usable!=b->first_usable || a->last_usable!=b->last_usable ||
       a->entry_count!=b->entry_count || a->entry_size!=b->entry_size ||
       a->array_crc!=b->array_crc || !equal(a->disk_guid,b->disk_guid,16))
        return SWITCH_GPT_MISMATCH;
    uint32_t bytes=a->entry_count*a->entry_size;
    uint32_t crcs[2]={0xffffffffu,0xffffffffu},used=0,found=0,index=0;
    for(uint32_t offset=0;offset<bytes;offset+=512) {
        uint32_t n=bytes-offset;
        if(n>512) n=512;
        for(uint32_t i=0;i<2;i++) {
            rc=read_sector(read512,context,capacity,w->headers[i].entry_lba+offset/512,w->sectors[i]);
            if(rc) return rc;
            crcs[i]=crc_update(crcs[i],w->sectors[i],n);
        }
        if(!equal(w->sectors[0],w->sectors[1],n)) return SWITCH_GPT_MISMATCH;
        for(uint32_t pos=0;pos<n;pos+=a->entry_size) {
            rc=parse_entry(w->sectors[0]+pos,index++,expected,w,&used,&found);
            if(rc) return rc;
        }
    }
    if((crcs[0]^0xffffffffu)!=a->array_crc || (crcs[1]^0xffffffffu)!=b->array_crc)
        return SWITCH_GPT_ARRAY_CRC;
    if(!found) return SWITCH_GPT_NOT_FOUND;
    /* Assign fields explicitly so padding in caller output is untouched. */
    output->first_sector=w->candidate.first_sector;
    output->sector_count=w->candidate.sector_count;
    output->partition_index=w->candidate.partition_index;
    copy(output->unique_guid,w->candidate.unique_guid,16);
    return SWITCH_GPT_OK;
}
