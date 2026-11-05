// SPDX-License-Identifier: GPL-2.0-only
#include "install.h"
#include "../storage/raw_apfs.h"
#include <string.h>
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t le64(const uint8_t *p) { return le32(p)|(uint64_t)le32(p+4)<<32; }
static int zero(const uint8_t *p, uint32_t n) { while(n--) if(*p++) return 0; return 1; }
static int path_ok(const uint8_t *p) {
    uint32_t n=0,start=0;
    for(;n<SWITCH_INSTALL_PATH_BYTES && p[n];n++) {
        if(p[n]<32 || p[n]>126 || p[n]==':' || p[n]=='\\') return 0;
        if(p[n]=='/') {
            if(n==start || (n-start==1 && p[start]=='.') ||
               (n-start==2 && p[start]=='.' && p[start+1]=='.')) return 0;
            start=n+1;
        }
    }
    return n<SWITCH_INSTALL_PATH_BYTES && n>start &&
        !(n-start==1 && p[start]=='.') && !(n-start==2 && p[start]=='.' && p[start+1]=='.') &&
        zero(p+n,SWITCH_INSTALL_PATH_BYTES-n);
}
static int extents_ok(const struct switch_install_image *image) {
    if(!image->extent_count || image->extent_count>SWITCH_INSTALL_MAX_EXTENTS ||
       image->block_size<4096 || image->block_size>65536 ||
       (image->block_size&(image->block_size-1)) || image->bytes%image->block_size ||
       !image->source_bytes || image->source_bytes>image->bytes ||
       image->source_bytes%image->block_size) return 0;
    uint64_t image_end=0,source_end=0,copied=0;
    for(uint32_t i=0;i<image->extent_count;i++) {
        const struct switch_install_extent *e=&image->extents[i];
        if(!e->bytes || (e->image_offset%image->block_size) ||
           (e->source_offset%image->block_size) || (e->bytes%image->block_size) ||
           e->image_offset<image_end || e->image_offset>image->bytes ||
           e->bytes>image->bytes-e->image_offset ||
           e->source_offset<source_end || e->source_offset>image->source_bytes ||
           e->bytes>image->source_bytes-e->source_offset ||
           (!i && (e->image_offset || e->source_offset || e->bytes<image->block_size))) return 0;
        image_end=e->image_offset+e->bytes;source_end=e->source_offset+e->bytes;
        copied+=e->bytes;
    }
    return copied==image->copy_bytes;
}
int switch_install_manifest(const void *data, uint32_t size, struct switch_install_image *out) {
    if(!data || !out || size<64) return 1;
    const uint8_t *p=data;
    uint32_t count=le32(p+12),block=le32(p+24),version=le32(p+8),extents=le32(p+28);
    uint64_t bytes=le64(p+16),sum=0,source_bytes=bytes;
    if(!count || count>SWITCH_INSTALL_MAX_PARTS || block<4096 || block>65536 ||
       (block&(block-1)) || !bytes || bytes%block || bytes/512>UINT32_MAX) return 1;
    if(version==1) {
        if(memcmp(p,"SWCHIMG1",8) || extents || size!=64+count*256 || zero(p+32,32)) return 1;
        extents=1;
    } else if(version==2) {
        source_bytes=le64(p+32);
        if(memcmp(p,"SWCHIMG2",8) || !extents || extents>SWITCH_INSTALL_MAX_EXTENTS ||
           size!=64+count*256+extents*24 || !source_bytes || source_bytes>bytes ||
           source_bytes%block || !zero(p+40,24)) return 1;
    } else return 1;
    memset(out,0,sizeof(*out));out->count=count;out->bytes=bytes;out->block_size=block;
    out->source_bytes=source_bytes;out->extent_count=extents;
    if(version==1)memcpy(out->sha256,p+32,32);
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *part=p+64+i*256;
        uint64_t n=le64(part+240);
        if(!path_ok(part) || !n || (n&511) || n>UINT32_C(0xfffffe00) ||
           !zero(part+248,8) || n>source_bytes-sum) return 1;
        for(uint32_t j=0;j<i;j++) if(!strcmp((const char *)part,out->parts[j].path)) return 1;
        memcpy(out->parts[i].path,part,240);out->parts[i].bytes=n;sum+=n;
    }
    if(sum!=source_bytes || out->parts[0].bytes<block)return 1;
    if(version==1) {
        out->extents[0]=(struct switch_install_extent){0,0,bytes};out->copy_bytes=bytes;
    } else {
        for(uint32_t i=0;i<extents;i++) {
            const uint8_t *e=p+64+count*256+i*24;
            out->extents[i]=(struct switch_install_extent){le64(e),le64(e+8),le64(e+16)};
            if(out->extents[i].bytes>bytes-out->copy_bytes)return 1;
            out->copy_bytes+=out->extents[i].bytes;
        }
    }
    return !extents_ok(out);
}
int switch_install_nxsb(const struct switch_install_image *image, const void *block, uint32_t bytes) {
    if(!image || !block || bytes<image->block_size || !image->block_size ||
       switch_raw_apfs_validate_nxsb(block,bytes,image->bytes/512)) return 1;
    const uint8_t *p=block;
    return le32(p+36)!=image->block_size || le64(p+40)!=image->bytes/image->block_size;
}
int switch_install_stream(const struct switch_install_image *image, const struct switch_install_io *io,
    uint64_t first, uint64_t sectors, void *source, uint32_t size) {
    if(!image || !io || !io->ready || !io->source || !io->transfer || !io->sync ||
       !source || !size || (size&511) || size>SWITCH_INSTALL_CHUNK || size<image->block_size ||
       !image->bytes || (image->bytes&511) || image->bytes/512>sectors ||
       first>UINT32_MAX || sectors>UINT32_MAX-first || !extents_ok(image)) return 1;
    uint64_t copied=0;
    if(io->progress) io->progress(io->context,0,image->copy_bytes,1);
    for(uint32_t i=0;i<image->extent_count;i++) {
        const struct switch_install_extent *e=&image->extents[i];
        for(uint64_t offset=0;offset<e->bytes;) {
            uint32_t n=e->bytes-offset<size ? (uint32_t)(e->bytes-offset) : size;
            if(io->ready(io->context) || io->source(io->context,e->source_offset+offset,source,n) ||
               io->ready(io->context) || (!copied && switch_install_nxsb(image,source,n))) return 1;
            uint64_t lba=first+(e->image_offset+offset)/512;
            if(io->ready(io->context) || io->transfer(io->context,1,lba,n/512,source) ||
               io->ready(io->context)) return 1;
            offset+=n;copied+=n;
            if(io->progress) io->progress(io->context,copied,image->copy_bytes,1);
        }
    }
    return io->ready(io->context) || io->sync(io->context);
}
