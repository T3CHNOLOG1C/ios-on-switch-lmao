// Host mock of the TegraGM20B user client (CONTRACT.md v1) for NVK's nvkmd_darwin, interposed over IOKit: a "GM20B" that
// accepts every call and completes every submission at once (its fence page shows the seqno as soon as Exec returns).
// Enough for vkCreateDevice, metal2vulkan's device open and shader/pipeline creation (NAK SM53); nothing is executed.
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "TegraGM20BABI.h"

#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

enum {
    kService = 0x7e57,
    kConn = 0x7e58,
    kFenceToken = 0xfe000001,
    kMaxBo = 65536
};

struct bo {
    int fd;
    uint64_t bytes;
    void *import;
};
static struct bo bos[kMaxBo];
static atomic_uint nbo = 1;
static uint32_t submitted[16];
static uint32_t *fence; /* the fence page, one uint32 per channel at slot*16 */
static atomic_ulong calls[16];

static io_service_t mock_get(mach_port_t main, CFDictionaryRef matching)
{
    if (matching)
        CFRelease(matching);
    return kService;
}

static kern_return_t mock_open(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c)
{
    if (s != kService || type != TegraGM20BOpenType)
        return kIOReturnBadArgument;
    *c = kConn;
    return KERN_SUCCESS;
}

static kern_return_t mock_close(io_connect_t c)
{
    return KERN_SUCCESS;
}

static kern_return_t mock_release(io_object_t o)
{
    return (o == kService || o == kConn) ? KERN_SUCCESS : IOObjectRelease(o);
}

static void *backing(uint32_t token, uint64_t *bytes, int *fd)
{
    if (token == kFenceToken) {
        *bytes = 4096;
        *fd = -1;
        return fence;
    }
    if (!token || token >= kMaxBo || !bos[token].bytes)
        return NULL;
    *bytes = bos[token].bytes;
    *fd = bos[token].fd;
    return bos[token].import ? bos[token].import : (void *)1;
}

static kern_return_t mock_call(mach_port_t c, uint32_t sel, const void *in, size_t insz, void *out, size_t *outsz)
{
    const uint32_t *w = in;
    if (insz >= 8 && w[0] == TegraGM20BTunnelMagic) {
        sel = w[1];
        in = w + 2;
        insz -= 8;
        w = in;
    }
    if (sel == TegraGM20BTunnelProbe)
        return KERN_SUCCESS;
    if (sel < 16)
        atomic_fetch_add(&calls[sel], 1);
    switch (sel) {
    case TegraGM20BMethodGetInfo: {
        struct TegraGM20BAbiInfo i = {.abi_version = TegraGM20BAbiVersion,
                                      .flags = TegraGM20BInfoHostImport,
                                      .chipset = 0x12b,
                                      .gpc_count = 1,
                                      .tpc_count = 2,
                                      .cls_copy = 0xb0b5,
                                      .cls_eng2d = 0x902d,
                                      .cls_eng3d = 0xb197,
                                      .cls_m2mf = 0xa140,
                                      .cls_compute = 0xb1c0,
                                      .cls_gpfifo = 0xb06f,
                                      .va_start = 0x1000,
                                      .va_end = 1ull << 39,
                                      .page_size = 4096,
                                      .big_page_size = 0x20000,
                                      .max_bo_bytes = 256ull << 20};
        strcpy(i.chip_name, "GM20B");
        strcpy(i.device_name, "NVIDIA Tegra X1 (GM20B)");
        if (*outsz != sizeof(i))
            return kIOReturnBadArgument;
        memcpy(out, &i, sizeof(i));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const struct TegraGM20BAbiBoAlloc *a = in;
        unsigned h = atomic_fetch_add(&nbo, 1);
        char name[64];
        snprintf(name, sizeof(name), "/nvkmock.%d.%u", getpid(), h);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (fd < 0 || ftruncate(fd, (off_t)a->bytes))
            return kIOReturnNoMemory;
        bos[h] = (struct bo){fd, a->bytes, NULL};
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = h};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceWrites:
        return KERN_SUCCESS;
    case TegraGM20BMethodBoImport: {
        const struct TegraGM20BAbiBoImport *a = in;
        unsigned h = atomic_fetch_add(&nbo, 1);
        bos[h] = (struct bo){-1, a->bytes, (void *)(uintptr_t)a->address};
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = 0};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree: {
        const struct TegraGM20BAbiBoFree *a = in;
        if (a->handle < kMaxBo && bos[a->handle].fd >= 0)
            close(bos[a->handle].fd);
        if (a->handle < kMaxBo)
            bos[a->handle] = (struct bo){0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind:
    case TegraGM20BMethodChannelDestroy:
        return KERN_SUCCESS;
    case TegraGM20BMethodChannelCreate: {
        static uint32_t next = 1;
        if (!fence)
            fence = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        uint32_t ch = next++;
        struct TegraGM20BAbiChannelCreateReply r = {.channel = ch, .fence_token = kFenceToken, .fence_offset = ch * 16};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodExec: {
        const struct TegraGM20BAbiExec *e = in;
        uint32_t ch = e->channel & 15;
        if (e->count)
            submitted[ch]++;
        fence[ch * 4] = submitted[ch];
        struct TegraGM20BAbiExecReply r = {.seqno = submitted[ch]};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodWaitFence:
        return KERN_SUCCESS;
    case TegraGM20BMethodGetTimestamp: {
        struct TegraGM20BAbiTimestamp t = {.ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
        memcpy(out, &t, sizeof(t));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodGetStatus: {
        const struct TegraGM20BAbiChannel *ch = in;
        struct TegraGM20BAbiStatus s = {.completed = submitted[ch->channel & 15],
                                        .submitted = submitted[ch->channel & 15]};
        memcpy(out, &s, sizeof(s));
        return KERN_SUCCESS;
    }
    }
    return kIOReturnBadArgument;
}

static kern_return_t mock_map(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t *addr,
                              mach_vm_size_t *size, IOOptionBits opt)
{
    uint64_t bytes;
    int fd;
    void *b = backing(type, &bytes, &fd);
    if (!b)
        return kIOReturnBadArgument;
    if (type == kFenceToken && (opt & kIOMapAnywhere)) {
        *addr = (mach_vm_address_t)fence;
        *size = 4096;
        return KERN_SUCCESS;
    }
    void *at = (opt & kIOMapAnywhere) ? NULL : (void *)*addr;
    void *m = mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | (at ? MAP_FIXED : 0), fd, 0);
    if (m == MAP_FAILED)
        return kIOReturnNoMemory;
    *addr = (mach_vm_address_t)m;
    *size = bytes;
    return KERN_SUCCESS;
}

static kern_return_t mock_unmap(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t addr)
{
    uint64_t bytes;
    int fd;
    if (type == kFenceToken)
        return KERN_SUCCESS;
    if (!backing(type, &bytes, &fd))
        return KERN_SUCCESS;
    munmap((void *)addr, bytes);
    return KERN_SUCCESS;
}

void mock_iokit_report(void)
{
    static const char *names[] = {"",       "GetInfo",   "BoAlloc",       "BoImport",
                                  "BoFree", "VmBind",    "ChannelCreate", "ChannelDestroy",
                                  "Exec",   "WaitFence", "GetTimestamp",  "GetStatus"};
    for (int i = 1; i < 12; i++)
        if (calls[i])
            fprintf(stderr, "mock: %s x%lu\n", names[i], (unsigned long)calls[i]);
}

DYLD_INTERPOSE(mock_get, IOServiceGetMatchingService)
DYLD_INTERPOSE(mock_open, IOServiceOpen)
DYLD_INTERPOSE(mock_close, IOServiceClose)
DYLD_INTERPOSE(mock_release, IOObjectRelease)
DYLD_INTERPOSE(mock_call, IOConnectCallStructMethod)
DYLD_INTERPOSE(mock_map, IOConnectMapMemory64)
DYLD_INTERPOSE(mock_unmap, IOConnectUnmapMemory64)
