#include "data_backend_source.h"
#include "../IOSwitchWritableData.h"
#include <string.h>
#define DATA_MAGIC UINT64_C(0x494f535744415441)
#define DATA_MAP_LIMIT (16u * 1024u * 1024u)
struct iosw_data_backend {
    uint64_t magic;
    struct iosw_block_allocator allocator;
    struct iosw_data_source source;
    struct iosw_split_indexed_disk disk;
    uint32_t *map, *sorted;
    uint32_t count;
    size_t map_bytes;
    uint64_t partition_start, data_start;
    uint32_t generation, total_sectors, cluster_count, sectors_per_cluster;
    unsigned online;
};
static int contains(const uint32_t *a,uint32_t n,uint32_t key) {
    uint32_t lo=0,hi=n;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2;if(a[mid]<key)lo=mid+1;else hi=mid;}
    return lo<n && a[lo]==key;
}
static void sift(uint32_t *a,uint32_t root,uint32_t n) {
    while(root<n/2){uint32_t child=root*2+1;
        if(child+1<n && a[child]<a[child+1])child++;
        if(a[root]>=a[child])break;
        uint32_t v=a[root];a[root]=a[child];a[child]=v;root=child;}
}
static void sort(uint32_t *a,uint32_t n) {
    uint32_t at=1;while(at<n && a[at-1]<=a[at])at++;
    if(at>=n)return;
    for(uint32_t i=n/2;i;i--)sift(a,i-1,n);
    for(uint32_t i=n;i>1;i--){uint32_t v=a[0];a[0]=a[i-1];a[i-1]=v;sift(a,0,i-1);}
}
static int healthy(struct iosw_data_backend *b) {
    if(!b || b->magic!=DATA_MAGIC || !b->online)return 0;
    const struct iosw_fat_volume *v=b->source.volume;
    if(!v || !v->mounted || v!=b->disk.volume || v->generation!=b->generation ||
       v->partition_start!=b->partition_start || v->data_start!=b->data_start ||
       v->total_sectors!=b->total_sectors || v->cluster_count!=b->cluster_count ||
       v->sectors_per_cluster!=b->sectors_per_cluster ||
       b->source.ready(b->source.context)!=IOSW_BLOCK_OK) {
        b->online=0;b->source.offline(b->source.context);return 0;
    }
    return 1;
}
static int permit(void *ctx,uint64_t lba,uint32_t count) {
    struct iosw_data_backend *b=ctx;
    if(!healthy(b) || !count || count>128)return 0;
    const struct iosw_fat_volume *v=b->source.volume;
    if(lba<v->data_start || lba-v->data_start>=
       (uint64_t)v->cluster_count*v->sectors_per_cluster)return 0;
    uint64_t first=lba-v->data_start,limit=(uint64_t)v->cluster_count*v->sectors_per_cluster;
    if(count>limit-first)return 0;
    uint32_t a=(uint32_t)(first/v->sectors_per_cluster)+2;
    uint32_t z=(uint32_t)((first+count-1)/v->sectors_per_cluster)+2;
    for(uint32_t c=a;c<=z;c++)if(!contains(b->sorted,b->count,c))return 0;
    return 1;
}
void iosw_data_destroy(struct iosw_data_backend *b) {
    if(!b || b->magic!=DATA_MAGIC)return;
    struct iosw_block_allocator a=b->allocator;b->online=0;
    if(b->source.unbind)b->source.unbind(b->source.context,b);
    b->magic=0;
    if(b->map)a.release(a.context,b->map,b->map_bytes);
    if(b->sorted)a.release(a.context,b->sorted,b->map_bytes);
    a.release(a.context,b,sizeof(*b));
}
int iosw_data_create(const struct iosw_block_allocator *a,
    struct iosw_block_backend *read_only,struct iosw_data_backend **out) {
    if(!out)return IOSW_BLOCK_INVALID;*out=0;
    if(!a || !a->allocate || !a->release || !read_only)return IOSW_BLOCK_INVALID;
    struct iosw_data_source s={0};int status=ioswitch_files_data_source(&s);
    if(status)return status;
    if(!s.volume || !s.volume->mounted || !s.ready || !s.begin || !s.begin_metadata ||
       !s.end || !s.offline || !s.qualify || !s.unbind || !s.write || !s.sync)
        return IOSW_BLOCK_CORRUPT;
    if(IOSW_WRITABLE_DATA_PART_COUNT!=8 || IOSW_WRITABLE_DATA_BYTES!=UINT64_C(17179869184))
        return IOSW_BLOCK_CORRUPT;
    struct iosw_split_part parts[IOSW_WRITABLE_DATA_PART_COUNT];
    for(unsigned p=0;p<IOSW_WRITABLE_DATA_PART_COUNT;p++)parts[p]=(struct iosw_split_part){
        iosw_writable_data_parts[p].path,iosw_writable_data_parts[p].offset,iosw_writable_data_parts[p].size};
    uint32_t count=0;
    if(iosw_split_index_count(s.volume,parts,IOSW_WRITABLE_DATA_PART_COUNT,IOSW_WRITABLE_DATA_BYTES,&count) ||
       !count || count>DATA_MAP_LIMIT/sizeof(uint32_t))return IOSW_BLOCK_CORRUPT;
    struct iosw_data_backend *b=a->allocate(a->context,sizeof(*b));if(!b)return IOSW_BLOCK_NOMEM;
    memset(b,0,sizeof(*b));b->magic=DATA_MAGIC;b->allocator=*a;b->source=s;b->count=count;
    b->partition_start=s.volume->partition_start;b->data_start=s.volume->data_start;
    b->generation=s.volume->generation;b->total_sectors=s.volume->total_sectors;
    b->cluster_count=s.volume->cluster_count;b->sectors_per_cluster=s.volume->sectors_per_cluster;
    b->map_bytes=(size_t)count*sizeof(uint32_t);
    b->map=a->allocate(a->context,b->map_bytes);b->sorted=a->allocate(a->context,b->map_bytes);
    if(!b->map || !b->sorted){iosw_data_destroy(b);return IOSW_BLOCK_NOMEM;}
    status=s.begin_metadata(s.context,131072,120);
    if(status){iosw_data_destroy(b);return status;}
    int error=iosw_split_open_indexed(&b->disk,s.volume,parts,IOSW_WRITABLE_DATA_PART_COUNT,
        IOSW_WRITABLE_DATA_BYTES,b->map,count);
    int end=s.end(s.context);
    if(error || end){status=end?end:error==IOSW_FAT_IO?IOSW_BLOCK_IO:IOSW_BLOCK_CORRUPT;goto failure;}
    memcpy(b->sorted,b->map,b->map_bytes);sort(b->sorted,count);
    status=iosw_block_excludes_clusters(read_only,s.volume,b->sorted,count);
    if(status)goto failure;
    /* All mappings come from this boot's FAT filesystem. Validate every
     * selected ancestor directory against Data before enabling any writes. */
    status=s.begin_metadata(s.context,131072,120);
    if(status)goto failure;
    for(unsigned p=0;p<IOSW_WRITABLE_DATA_PART_COUNT && !status;p++) {
        error=iosw_fat_path_directories_exclude(s.volume,parts[p].path,b->sorted,count);
        if(error)status=error==IOSW_FAT_IO?IOSW_BLOCK_IO:IOSW_BLOCK_CORRUPT;
    }
    if(!status)status=iosw_block_excludes_directories(read_only,s.volume,b->sorted,count);
    end=s.end(s.context);
    if(end)status=end;
    if(status)goto failure;
    /* No external client can call the permission function until qualification
     * installs it, and no disk is published until qualification succeeds. */
    b->online=1;
    status=s.qualify(s.context,permit,b);
    if(status)goto failure;
    *out=b;return IOSW_BLOCK_OK;
failure:
    s.offline(s.context);iosw_data_destroy(b);return status;
}
int iosw_data_get_info(struct iosw_data_backend *b,struct iosw_block_info *info) {
    if(!info)return IOSW_BLOCK_INVALID;memset(info,0,sizeof(*info));
    if(!healthy(b))return IOSW_BLOCK_NOT_READY;
    info->size_bytes=IOSW_WRITABLE_DATA_BYTES;memcpy(info->label,"Data",5);return IOSW_BLOCK_OK;
}
static int request(struct iosw_data_backend *b,uint64_t offset,const void *bytes,uint32_t length) {
    if(!bytes || !length || length>IOSW_BLOCK_MAX_TRANSFER_BYTES || ((offset|length)&511u) ||
       offset>IOSW_WRITABLE_DATA_BYTES || length>IOSW_WRITABLE_DATA_BYTES-offset)return IOSW_BLOCK_INVALID;
    return healthy(b)?IOSW_BLOCK_OK:IOSW_BLOCK_NOT_READY;
}
static int finish(struct iosw_data_backend *b,int error) {
    int end=b->source.end(b->source.context);int status=end?end:error;
    if(status){b->online=0;b->source.offline(b->source.context);}return status;
}
int iosw_data_read(struct iosw_data_backend *b,uint64_t offset,void *bytes,uint32_t length) {
    int status=request(b,offset,bytes,length);if(status)return status;
    status=b->source.begin(b->source.context,length/512,5);if(status)return status;
    int error=iosw_split_read_indexed(&b->disk,offset,bytes,length);
    return finish(b,error==IOSW_FAT_IO?IOSW_BLOCK_IO:error?IOSW_BLOCK_CORRUPT:IOSW_BLOCK_OK);
}
int iosw_data_write(struct iosw_data_backend *b,uint64_t offset,const void *bytes,uint32_t length) {
    int status=request(b,offset,bytes,length);if(status)return status;
    status=b->source.begin(b->source.context,length/512,30);if(status)return status;
    const uint8_t *in=bytes;struct iosw_fat_volume *v=b->source.volume;
    uint32_t cluster_bytes=v->sectors_per_cluster*512;
    while(length) {
        unsigned p=0;while(p+1<b->disk.count && offset>=b->disk.offsets[p+1])p++;
        const struct iosw_fat_indexed_file *f=&b->disk.files[p];
        uint64_t at=offset-b->disk.offsets[p];uint32_t index=(uint32_t)(at/cluster_bytes);
        if(index>=f->count){status=IOSW_BLOCK_CORRUPT;break;}
        uint32_t within=(uint32_t)(at%cluster_bytes)/512;
        uint64_t lba=v->data_start+(uint64_t)(f->clusters[index]-2)*v->sectors_per_cluster+within;
        uint32_t sectors=v->sectors_per_cluster-within,available=length/512;
        uint32_t file_remaining=(uint32_t)((f->file.size-at)/512);
        if(available>file_remaining)available=file_remaining;
        if(available>128)available=128;
        if(sectors>available)sectors=available;
        while(sectors<available && index+1<f->count && f->clusters[index+1]==f->clusters[index]+1) {
            uint32_t n=v->sectors_per_cluster;if(n>available-sectors)n=available-sectors;
            sectors+=n;index++;
        }
        if(!sectors || !permit(b,lba,sectors)){status=IOSW_BLOCK_CORRUPT;break;}
        status=b->source.write(b->source.context,lba,sectors,in);if(status)break;
        uint32_t n=sectors*512;offset+=n;in+=n;length-=n;
    }
    return finish(b,status);
}
int iosw_data_sync(struct iosw_data_backend *b) {
    if(!healthy(b))return IOSW_BLOCK_NOT_READY;
    int status=b->source.begin(b->source.context,1,30);if(status)return status;
    return finish(b,b->source.sync(b->source.context));
}
