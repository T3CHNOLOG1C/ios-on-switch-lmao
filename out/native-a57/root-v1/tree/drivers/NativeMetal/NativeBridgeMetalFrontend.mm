// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: indirect command buffers, visible function
// tables, function stitching, drawables/presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"

static NmbDevice *gDevice;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits, gCommitFailures, gPipelines, gProblems, gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses, gDraws, gPassPixels, gDispatches, gBlits, gBiggestPass, gFullScreenPasses;
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}
struct Timed {
    std::atomic<uint64_t> count{0}, total{0}, slowest{0};
    void add(uint64_t began)
    {
        const uint64_t took = nowUs() - began;
        count++;
        total += took;
        uint64_t seen = slowest.load();
        while (took > seen && !slowest.compare_exchange_weak(seen, took)) {}
    }
};
static Timed gTimePipeline, gTimeLibrary, gTimeFunction, gTimeEncodeToCommit;
static Timed gTimeWaitScheduled, gTimeWaitCompleted;
static Timed gTimeResourceWait;
// r6-lows: per-command-buffer time distributions for the rates line (the kernel's display stats line has the presents): fixed bins
// (1 ms to 64 ms, 4 ms to 128, 8 ms to 256, 64 ms to 1024, one beyond) of relaxed atomics, added from any thread, read and cleared
// by native_bridge_metal_rates. A quantile is its bin's upper edge, capped at the window's maximum.
struct NBHist {
    static constexpr unsigned kBins = 64 + 16 + 16 + 12 + 1;
    std::atomic<uint32_t> bins[kBins];
    std::atomic<uint64_t> top{0};
    static unsigned binOf(uint64_t us)
    {
        const uint64_t ms = us / 1000;
        if (ms < 64) return (unsigned)ms;
        if (ms < 128) return 64 + (unsigned)((ms - 64) / 4);
        if (ms < 256) return 80 + (unsigned)((ms - 128) / 8);
        if (ms < 1024) return 96 + (unsigned)((ms - 256) / 64);
        return kBins - 1;
    }
    static uint64_t upperUs(unsigned bin)
    {
        if (bin < 64) return (bin + 1) * 1000ull;
        if (bin < 80) return (64 + (bin - 63) * 4) * 1000ull;
        if (bin < 96) return (128 + (bin - 79) * 8) * 1000ull;
        if (bin < 108) return (256 + (bin - 95) * 64) * 1000ull;
        return UINT64_MAX;
    }
    void add(uint64_t us)
    {
        bins[binOf(us)].fetch_add(1, std::memory_order_relaxed);
        uint64_t seen = top.load(std::memory_order_relaxed);
        while (us > seen && !top.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
    }
    // The window's counts, cleared: how many, p50, p99 and the maximum (microseconds), and how many reached the bin of overUs, and
    // of over2Us, or beyond.
    struct Taken {
        uint64_t n, p50, p99, max, over, over2;
    };
    Taken take(uint64_t overUs, uint64_t over2Us)
    {
        uint32_t counts[kBins];
        uint64_t n = 0;
        for (unsigned bin = 0; bin < kBins; bin++) {
            counts[bin] = bins[bin].exchange(0, std::memory_order_relaxed);
            n += counts[bin];
        }
        const uint64_t most = top.exchange(0, std::memory_order_relaxed);
        auto quantile = [&](unsigned permille) -> uint64_t {
            if (!n) return 0;
            const uint64_t rank = (n * permille + 999) / 1000;
            uint64_t seen = 0;
            for (unsigned bin = 0; bin < kBins; bin++) {
                seen += counts[bin];
                if (seen >= rank) {
                    const uint64_t up = upperUs(bin);
                    return up < most ? up : most;
                }
            }
            return most;
        };
        uint64_t over = 0, over2 = 0;
        for (unsigned bin = binOf(overUs); bin < kBins; bin++) over += counts[bin];
        for (unsigned bin = binOf(over2Us); bin < kBins; bin++) over2 += counts[bin];
        return {n, quantile(500), quantile(990), most, over, over2};
    }
};
// enc: creation to commit (the encoding thread's wall time for one command buffer); gap: one committing thread's commit to its
// next (CA's render server commits its frames from one thread; gaps of 250 ms or more are idle and not counted); sched: each
// waitUntilScheduled (CA waits for it before it swaps); gpu: commit to completion; pso: each render pipeline state made.
static NBHist gHistEncode, gHistGap, gHistSched, gHistGpu, gHistPso;
static thread_local uint64_t tLastCommitUs;
static std::atomic<uint64_t> gResourceChecks, gResourceSkipped;
// The rest of the 5 s rates line (native_bridge_metal_rates): the runtime's commit call (submission, and any CPU wait inside it), each
// command buffer's encoding (creation to commit, wall time), the time its encoding thread spent inside this frontend's encoding calls
// (apiTime: what of the encoding is ours, the rest being the caller's own work between calls), and the render encoders that open on
// exactly the attachments the command buffer's previous encoder rendered, loading what it stored (passes one pass could hold).
static Timed gTimeSubmit;
static std::atomic<uint64_t> gEncodeUs, gApiUs, gJoinablePasses;
// Mach ticks spent in the encoding calls on this thread since its last commit (one thread encodes a command buffer, Metal's rule; the
// committing thread hands its sum to the rates). Off with TEGRA_M2V_TELEMETRY=0 (the plugin sets it from tegra-m2v-telemetry).
static thread_local uint64_t tApiTicks;
static bool apiTiming()
{
    static const bool on = [] {
        const char *v = getenv("TEGRA_M2V_TELEMETRY");
        return !v || strcmp(v, "0");
    }();
    return on;
}
struct NBApiTime {
    const uint64_t began = apiTiming() ? mach_absolute_time() : 0;
    ~NBApiTime()
    {
        if (began) tApiTicks += mach_absolute_time() - began;
    }
};
static uint64_t ticksToUs(uint64_t ticks)
{
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b{};
        mach_timebase_info(&b);
        return b;
    }();
    return base.denom ? ticks * base.numer / base.denom / 1000 : 0;
}
// replaceRegion/getBytes calls (the runtime makes some of them an internal submission it waits for: a CPU wait for the GPU).
static Timed gTimeTexelCopy;
// Planar IOSurfaces a texture was asked over (refused), and the device answers asked of the runtime and kept (askAnswer).
static std::atomic<uint64_t> gSurfaceMultiPlane;
// Host imports the GPU wrote, invalidated for this process's CPU readback (invalidateForCpu): count, bytes, microseconds.
static std::atomic<uint64_t> gReadbackInvalidates, gReadbackBytes, gReadbackUs;
// Surface imports (surfaceBuffer): new ones, timed (the runtime's host import: the kernel's BoImport and VmBind, the memory and
// its block), and lookups an import still alive answered; textures made over a surface import, timed (the runtime's image).
static Timed gTimeSurfaceImport, gTimeSurfaceTexture;
static std::atomic<uint64_t> gSurfaceImportHits;
static std::atomic<uint64_t> gAnswersAsked, gAnswersKept;
struct EncodedWork {
    uint32_t passes, draws, dispatches, blits;
    uint64_t pixels;
};
struct CommitRecord {
    uint64_t committedUs = 0, callbackUs = 0, ageUs = 0;
    int64_t scheduledUs = -1;
    uint32_t index = 0;
    EncodedWork work = {};
};
static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
static std::mutex gProblemLock;
static char gLastProblems[2][200];
static void noteProblem(NSString *what)
{
    gProblems++;
    std::lock_guard<std::mutex> lock(gProblemLock);
    memcpy(gLastProblems[1], gLastProblems[0], sizeof gLastProblems[0]);
    strlcpy(gLastProblems[0], what.UTF8String ?: "?", sizeof gLastProblems[0]);
}
#define NBLOG(...)                                                  \
    do {                                                            \
        NSString *nbText = [NSString stringWithFormat:__VA_ARGS__]; \
        NSLog(@"%@", nbText);                                       \
        noteProblem(nbText);                                        \
    } while (0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];
static unsigned gFormatCount;
static std::atomic<uint64_t> gTextureRefusals;
static void noteFormat(uint32_t format, bool made)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    if (!made) gTextureRefusals++;
    for (unsigned i = 0; i < gFormatCount; i++)
        if (gFormats[i] == format) return;
    if (gFormatCount < 8) gFormats[gFormatCount++] = format;
}
static NSError *nbError(NSString *why)
{
    return [NSError errorWithDomain:@"org.blu-sharky.native-metal"
                               code:-1
                           userInfo:@{NSLocalizedDescriptionKey : why ?: @"refused"}];
}
static NSString *lastError()
{
    return [NSString stringWithUTF8String:nmb_last_error()];
}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static void journal(const std::string &line)
{
    if (gJournalPath.empty()) return;
    std::lock_guard<std::mutex> lock(gJournalLock);
    if (FILE *f = fopen(gJournalPath.c_str(), "a")) {
        fwrite(line.data(), 1, line.size(), f);
        fputc('\n', f);
        fclose(f);
    }
}

@interface NBObject : NSObject
@property (atomic, copy) NSString *label;
@end
@implementation NBObject
- (id<MTLDevice>)device
{
    return gDeviceObject;
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    if (gMissing) gMissing(class_getName(object_getClass(self)), sel_getName(selector));
    noteProblem(
        [NSString stringWithFormat:@"missing %s %s", class_getName(object_getClass(self)), sel_getName(selector)]);
    [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI                             \
    -(uint64_t)protectionOptions                    \
    {                                               \
        return 0;                                   \
    }                                               \
    -(id<MTLHeap>)heap                              \
    {                                               \
        return _parentHeap;                         \
    }                                               \
    -(BOOL)isAliasable                              \
    {                                               \
        return NO;                                  \
    }                                               \
    -(int)setOwnerWithIdentity : (unsigned)identity \
    {                                               \
        (void)identity;                             \
        return 46; /* KERN_NOT_SUPPORTED */         \
    }                                               \
    -(void)waitUntilComplete                        \
    {                                               \
        waitForResource(self);                      \
    }                                               \
    -(void)makeAliasable {} /* the runtime never reuses a heap resource's memory: keeping it is always correct */
@interface NBBuffer : NBObject
@property (nonatomic) NmbBuffer *handle;
@property (nonatomic) MTLResourceOptions options;
@property (nonatomic, copy) void (^deallocator)(void *, NSUInteger);
@property (nonatomic) void *wrapped;
@property (nonatomic) IOSurfaceRef surfaceOwner;  // keeps a shared import's CPU mapping alive
@property (nonatomic)
    uint32_t gpuDirty;  // the GPU wrote this host import since its lines were last invalidated (invalidateForCpu)
@property (nonatomic, strong) id<MTLHeap> parentHeap;  // the heap that made the buffer, nil otherwise
@property (nonatomic) NSUInteger heapOffset;
- (bool)invalidateForCpu;
- (void)gpuWrote;
@end
@interface NBTexture : NBObject
@property (nonatomic) NmbTexture *handle;
@property (nonatomic) NmbTextureDescriptor descriptor;
@property (nonatomic, strong) NBBuffer *buffer;
@property (nonatomic) IOSurfaceRef surface;
@property (nonatomic) NSUInteger offset, rowBytes;
@property (nonatomic, strong) id<MTLTexture> parent;
@property (nonatomic, strong) id<MTLHeap> parentHeap;
@property (nonatomic) NSUInteger heapOffset;
- (NBBuffer *)hostImport;
@end
// Clean [base, base + bytes) to the point of coherency and invalidate it, by VA from EL0 (XNU sets SCTLR_EL1.UCI). Cortex-A57 data
// cache lines are 64 bytes (CTR_EL0 is not readable at EL0 here: SCTLR_EL1.UCT is clear). The caller ends its run of these with a DSB.
static void cleanToCoherency(const void *base, size_t bytes)
{
    uintptr_t line = (uintptr_t)base & ~(uintptr_t)63;
    const uintptr_t end = (uintptr_t)base + bytes;
    for (; line + 256 <= end; line += 256)
        __asm__ volatile("dc civac, %0\n dc civac, %1\n dc civac, %2\n dc civac, %3" ::"r"(line), "r"(line + 64),
                         "r"(line + 128), "r"(line + 192)
                         : "memory");
    for (; line < end; line += 64) __asm__ volatile("dc civac, %0" ::"r"(line) : "memory");
}
@implementation NBBuffer
// A host import the GPU wrote (gpuWrote, set at the writing command buffer's completion): invalidate its lines so a CPU read in
// this process reads what the GPU wrote, not lines it cached before (a fresh surface's zeros, a frame it read earlier). Lazy: the
// flag is set cheaply at completion and the invalidate paid here, only when the CPU reads (getBytes, contents), so a surface the
// CPU never reads (the framebuffer) costs nothing. The kernel's IOSurface-lock hook does the same for readers in other processes.
- (bool)invalidateForCpu
{
    if (!__atomic_exchange_n(&_gpuDirty, 0, __ATOMIC_ACQUIRE)) return false;
    const size_t bytes = nmb_buffer_length(_handle);
    const uint64_t began = nowUs();
    cleanToCoherency(nmb_buffer_contents(_handle), bytes);
    __asm__ volatile("dsb sy" ::: "memory");
    gReadbackInvalidates++;
    gReadbackBytes += bytes;
    gReadbackUs += nowUs() - began;
    return true;
}
- (void)gpuWrote
{
    __atomic_store_n(&_gpuDirty, 1, __ATOMIC_RELEASE);
}
- (void)dealloc
{
    const NSUInteger length = nmb_buffer_length(_handle);
    nmb_buffer_release(_handle);
    if (_deallocator) _deallocator(_wrapped, length);
    if (_surfaceOwner) CFRelease(_surfaceOwner);
}
- (NSUInteger)length
{
    return nmb_buffer_length(_handle);
}
- (void *)contents
{
    [self invalidateForCpu];
    return nmb_buffer_contents(_handle);
}  // a CPU read after a GPU write sees the GPU's bytes
- (uint64_t)gpuAddress
{
    return nmb_buffer_gpu_address(_handle);
}
- (MTLResourceOptions)resourceOptions
{
    return _options;
}
- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return (MTLHazardTrackingMode)((_options & MTLResourceHazardTrackingModeMask) >>
                                   MTLResourceHazardTrackingModeShift);
}
- (void)didModifyRange:(NSRange)range
{
    (void)range;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_buffer_set_purgeable_state(_handle, state);
    if (!answer) NBLOG(@"native-metal: buffer setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize
{
    return nmb_buffer_length(_handle);
}
- (IOSurfaceRef)iosurface
{
    return nullptr;
}  // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *, NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
// Associations kept before the dead ones are dropped. Above the live imports a busy screen holds (r5c: 307), so a live import's
// association is never the one dropped: a lookup that missed it imported the same surface a second time.
static constexpr NSUInteger kSurfaceImportKeys = 1024;
static NBBuffer *surfaceBuffer(IOSurfaceRef surface, void *base, size_t bytes)
{
    struct Key {
        uint64_t identity, address, bytes;
    };
    const Key value = {IOSurfaceGetID(surface), (uintptr_t)base, bytes};
    NSValue *key = value.identity ? [NSValue valueWithBytes:&value objCType:@encode(Key)] : nil;
    std::lock_guard<std::mutex> lock(gSurfaceImportLock);
    if (!gSurfaceImports) gSurfaceImports = [NSMapTable strongToWeakObjectsMapTable];
    NBBuffer *buffer = key ? [gSurfaceImports objectForKey:key] : nil;
    if (buffer) {
        gSurfaceImportHits++;
        return buffer;
    }
    const uint64_t began = nowUs();
    NmbBuffer *handle = nmb_buffer_new_no_copy(gDevice, base, bytes, MTLResourceStorageModeShared);
    gTimeSurfaceImport.add(began);
    if (!handle) {
        NBLOG(@"native-metal: surface import refused: %@", lastError());
        return nil;
    }
    buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = MTLResourceStorageModeShared;
    buffer.surfaceOwner = (IOSurfaceRef)CFRetain(surface);
    if (key) {
        // Bound metadata without retaining old frames. Evicting an association is
        // harmless: its owners still retain the actual import until their own end.
        if (gSurfaceImports.count >= kSurfaceImportKeys) {
            for (NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
                if (![gSurfaceImports objectForKey:old]) [gSurfaceImports removeObjectForKey:old];
            if (gSurfaceImports.count >= kSurfaceImportKeys)
                [gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
        }
        [gSurfaceImports setObject:buffer forKey:key];
    }
    return buffer;
}
static bool textureDescriptor(MTLTextureDescriptor *d, NmbTextureDescriptor *out)
{
    if (!d) return false;
    *out = {(uint64_t)d.textureType,
            (uint64_t)d.pixelFormat,
            d.width,
            d.height,
            d.depth,
            d.mipmapLevelCount,
            d.sampleCount,
            d.arrayLength,
            (uint64_t)d.usage,
            (uint64_t)d.resourceOptions};
    return true;
}
@implementation NBTexture
- (void)dealloc
{
    nmb_texture_release(_handle);
    if (_surface) CFRelease(_surface);
}
- (MTLTextureType)textureType
{
    return (MTLTextureType)_descriptor.type;
}
- (MTLPixelFormat)pixelFormat
{
    return (MTLPixelFormat)_descriptor.format;
}
- (NSUInteger)width
{
    return _descriptor.width;
}
- (NSUInteger)height
{
    return _descriptor.height;
}
- (NSUInteger)depth
{
    return _descriptor.depth;
}
- (NSUInteger)mipmapLevelCount
{
    return _descriptor.levels;
}
- (NSUInteger)sampleCount
{
    return _descriptor.samples;
}
- (NSUInteger)arrayLength
{
    return _descriptor.array_length;
}
- (MTLTextureUsage)usage
{
    return (MTLTextureUsage)_descriptor.usage;
}
- (MTLResourceOptions)resourceOptions
{
    return (MTLResourceOptions)_descriptor.options;
}
- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_descriptor.options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_descriptor.options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}
- (BOOL)isFramebufferOnly
{
    return NO;
}
- (IOSurfaceRef)iosurface
{
    return _surface;
}
- (NSUInteger)iosurfacePlane
{
    return 0;
}
- (id<MTLBuffer>)buffer
{
    return _surface ? nil : (id<MTLBuffer>)_buffer;
}
// The host import whose pages hold this texture's texels (a surface's, or a no-copy buffer's), for the lazy readback invalidate.
- (NBBuffer *)hostImport
{
    return (_surface || (_buffer && _buffer.wrapped)) ? _buffer : nil;
}
- (NSUInteger)bufferOffset
{
    return _offset;
}
- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}
- (id<MTLTexture>)parentTexture
{
    return _parent;
}
- (NSUInteger)rotation
{
    return 0;
}
// Nothing here is compressed (NVK on GM20B has no compression) or a drawable; views take no swizzle (identity).
- (MTLTextureCompressionType)compressionType
{
    return MTLTextureCompressionTypeLossless;
}
- (BOOL)isCompressed
{
    return NO;
}
- (BOOL)isDrawable
{
    return NO;
}
- (MTLTextureSwizzleChannels)swizzle
{
    return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                         MTLTextureSwizzleAlpha);
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_texture_set_purgeable_state(_handle, state);
    if (!answer) NBLOG(@"native-metal: texture setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI
- (id<MTLTexture>)view:(NmbTexture *)handle
{
    if (!handle) {
        NBLOG(@"native-metal: texture view refused: %@", lastError());
        return nil;
    }
    NBTexture *view = [NBTexture new];
    view.handle = handle;
    NmbTextureDescriptor descriptor;
    nmb_texture_describe(handle, &descriptor);
    view.descriptor = descriptor;
    view.parent = (id<MTLTexture>)self;
    view.buffer = _buffer;
    view.offset = _offset;
    view.rowBytes = _rowBytes;
    if (_surface) view.surface = (IOSurfaceRef)CFRetain(_surface);
    return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    return [self view:nmb_texture_new_view(_handle, format, 0, nullptr, nullptr, true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
{
    const uint64_t l[2] = {levels.location, levels.length}, s[2] = {slices.location, slices.length};
    return [self view:nmb_texture_new_view(_handle, format, type, l, s, false)];
}
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    NBApiTime timed;
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    const uint64_t length = (r.size.depth > 1 ? image * (r.size.depth - 1) : 0) + row * r.size.height;
    const uint64_t began = nowUs();
    const bool replaced = nmb_texture_replace(_handle, region, level, slice, bytes, row, image, length);
    gTimeTexelCopy.add(began);
    if (!replaced) NBLOG(@"native-metal: replaceRegion refused: %@", lastError());
}
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row * r.size.height];
}
- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    const uint64_t length = (r.size.depth > 1 ? image * (r.size.depth - 1) : 0) + row * r.size.height;
    [[self hostImport] invalidateForCpu];  // a read back of what the GPU wrote this texture's surface/buffer pages
    const uint64_t began = nowUs();
    const bool read = nmb_texture_get_bytes(_handle, bytes, row, image, length, region, level, slice);
    gTimeTexelCopy.add(began);
    if (!read) NBLOG(@"native-metal: getBytes refused: %@", lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:row * r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// The host import whose pages hold `owner`'s texels (a surface- or no-copy-buffer-backed texture, or such a buffer), for the lazy
// readback invalidate: nil for an ordinary private/shared resource.
static NBBuffer *hostImportOf(id owner)
{
    if ([owner isKindOfClass:[NBTexture class]]) return [(NBTexture *)owner hostImport];
    if ([owner isKindOfClass:[NBBuffer class]] && ([(NBBuffer *)owner surfaceOwner] || [(NBBuffer *)owner wrapped]))
        return (NBBuffer *)owner;
    return nil;
}
static NBTexture *textureOnBuffer(NBBuffer *buffer, MTLTextureDescriptor *d, NSUInteger offset, NSUInteger row,
                                  bool surface)
{
    NmbTextureDescriptor descriptor;
    if (!buffer || !textureDescriptor(d, &descriptor)) return nil;
    const uint64_t began = nowUs();
    NmbTexture *handle = nmb_texture_new_on_buffer(buffer.handle, &descriptor, offset, row, surface);
    if (surface) gTimeSurfaceTexture.add(began);
    if (!handle) {
        NBLOG(@"native-metal: texture over buffer refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = buffer;
    texture.offset = offset;
    texture.rowBytes = row;
    return texture;
}
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    return (id<MTLTexture>)textureOnBuffer(self, d, offset, row, false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor)) return nil;
    NmbTexture *handle = nmb_texture_new_linear(self.handle, &descriptor, offset, row, image);
    if (!handle) {
        NBLOG(@"native-metal: linear texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = self;
    texture.offset = offset;
    texture.rowBytes = row;
    return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property (nonatomic) NmbLibrary *handle;
@property (nonatomic, strong)
    id storage;  // keeps the metallib's bytes: its functions' bitcode, which stitching sends (functionBitcode)
@property (nonatomic) const uint8_t *bytes;
@property (nonatomic) size_t length;
@end
@interface NBFunction : NBObject
@property (nonatomic) NmbFunction *handle;
@property (nonatomic, copy) NSString *name;
@property (nonatomic) MTLFunctionType functionType;
@property (nonatomic)
    std::string constantsText;  // name:type:hex of each constant value it was specialized with (the journal's)
@property (nonatomic, strong) NBLibrary *library;  // the library it came from, whose bytes hold its bitcode
@end
@interface NBArgumentEncoder : NBObject
@property (nonatomic) NmbArgumentEncoder *handle;
@property (nonatomic, strong) id<MTLBuffer> argumentBuffer;  // the buffer its writes go to, kept while it is set
+ (id<MTLArgumentEncoder>)with:(NmbArgumentEncoder *)handle;
@end
// The function constants a function declares (MTLFunctionConstant's readable properties), for functionConstantsDictionary.
@interface NBFunctionConstant : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) MTLDataType type;
@property (nonatomic) NSUInteger index;
@property (nonatomic) BOOL required;
@end
@implementation NBFunctionConstant
@end
@implementation NBFunction
- (void)dealloc
{
    nmb_function_release(_handle);
}
// MPS reads its kernels' constants (kMPSDeviceCaps, MPSMaxBatchSize, ...) here before specializing them.
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary
{
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    for (uint32_t i = 0, n = nmb_function_constant_count(_handle); i < n; i++) {
        char name[256];
        uint64_t info[3] = {};
        if (!nmb_function_constant(_handle, i, name, sizeof name, info)) continue;
        NBFunctionConstant *c = [NBFunctionConstant new];
        c.name = [NSString stringWithUTF8String:name];
        c.type = (MTLDataType)info[0];
        c.index = info[1];
        c.required = info[2] != 0;
        out[c.name] = c;
    }
    return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
}
- (MTLFunctionOptions)options
{
    return MTLFunctionOptionNone;
}
// The layout of the argument buffer the function takes at the index, as its AIR lays it out (the runtime's decode). No reflection
// object is made: the reflection form answers nil there, as Metal does when it was not asked for.
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    return [NBArgumentEncoder with:nmb_function_new_argument_encoder(_handle, (uint32_t)index)];
}
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                 reflection:(MTLAutoreleasedArgument *)reflection
{
    if (reflection) *reflection = nil;
    return [self newArgumentEncoderWithBufferIndex:index];
}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object, SEL selector, const char *encoding)
{
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    return method && !strcmp(method_getTypeEncoding(method), encoding);
}
// Bytes of a function constant of MTLDataType `type` (scalars and 2/4-wide vectors; 3-wide vectors are not qualified).
static uint64_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33: return 4;
    case 4:
    case 30:
    case 34: return 8;
    case 6:
    case 32:
    case 36: return 16;
    case 16:
    case 37:
    case 41: return 2;
    case 17:
    case 38:
    case 42: return 4;
    case 19:
    case 40:
    case 44: return 8;
    case 45:
    case 49:
    case 53: return 1;
    case 46:
    case 50:
    case 54: return 2;
    case 48:
    case 52:
    case 56: return 4;
    case 81:
    case 85: return 8;
    case 82:
    case 86: return 16;
    case 84:
    case 88: return 32;
    default: return 0;
    }
}
static std::string quartzCoreCallers();
static bool systemCompiler();
@implementation NBLibrary
- (void)dealloc
{
    nmb_library_release(_handle);
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    // No values are no constants set (Core Image asks its stitched kernels so), where Core Image's kernels are compiled.
    if (!values && systemCompiler()) values = [MTLFunctionConstantValues new];
    if (!name || !values || !methodABI(values, @selector(newIndexedConstantArray), "@16@0:8") ||
        !methodABI(values, @selector(newNamedConstantArray), "@16@0:8")) {
        if (error) *error = nbError(@"unqualified function constant values");
        return nil;
    }
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *indexed = [snapshot newIndexedConstantArray], *named = [snapshot newNamedConstantArray];
    std::vector<NmbConstant> constants;
    for (unsigned group = 0; group < 2; group++)
        for (id value in (group ? named : indexed)) {
            if (!methodABI(value, @selector(dataType), "Q16@0:8") || !methodABI(value, @selector(data), "^v16@0:8") ||
                !methodABI(value, group ? @selector(name) : @selector(index), group ? "@16@0:8" : "Q16@0:8")) {
                if (error) *error = nbError(@"unqualified constant value ABI");
                return nil;
            }
            id<NBOriginalConstantValue> v = value;
            const uint64_t bytes = constantBytes(v.dataType);
            if (!bytes || !v.data) {
                if (error)
                    *error = nbError(
                        [NSString stringWithFormat:@"constant type %lu is not qualified", (unsigned long)v.dataType]);
                return nil;
            }
            constants.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    std::string text;
    for (const NmbConstant &c : constants) {
        char head[32];
        if (!c.name) snprintf(head, sizeof head, "#%llu", (unsigned long long)c.index);
        text += text.empty() ? "" : " ";
        text += c.name ? c.name : head;
        snprintf(head, sizeof head, ":%llu:", (unsigned long long)c.type);
        text += head;
        for (uint64_t i = 0; i < c.length; i++) {
            snprintf(head, sizeof head, "%02x", ((const uint8_t *)c.bytes)[i]);
            text += head;
        }
    }
    const uint64_t functionBegan = nowUs();
    NmbFunction *handle =
        nmb_function_new_specialized(_handle, name.UTF8String, constants.data(), (uint32_t)constants.size());
    gTimeFunction.add(functionBegan);
    // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
    // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
    if (!gJournalPath.empty()) {
        char head[96];
        snprintf(head, sizeof head,
                 "S t=%llu us=%llu ok=%d name=", (unsigned long long)((functionBegan - gJournalBegan) / 1000),
                 (unsigned long long)(nowUs() - functionBegan), handle ? 1 : 0);
        journal(head + std::string(name.UTF8String ?: "?") + " k=" + text +
                (handle ? std::string() : " err=" + std::string(lastError().UTF8String ?: "?")) + " | " +
                quartzCoreCallers());
    }
    if (!handle) {
        noteProblem([NSString stringWithFormat:@"specialized function %@: %@", name, lastError()]);
        if (error) *error = nbError([NSString stringWithFormat:@"specialized function: %@", lastError()]);
        return nil;
    }
    NBFunction *function = [NBFunction new];
    function.handle = handle;
    function.name = name;
    function.functionType = (MTLFunctionType)nmb_function_type(handle);
    function.constantsText = text;
    function.library = self;
    if (error) *error = nil;
    return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithName:name constantValues:values error:&error];
    handler(function, error);
}
- (MTLLibraryType)type
{
    return MTLLibraryTypeExecutable;
}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if (!d.name) {
        if (error) *error = nbError(@"a function name is required");
        return nil;
    }
    if (d.constantValues) return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
    id<MTLFunction> function = [self newFunctionWithName:d.name];
    if (!function) {
        if (error) *error = nbError([NSString stringWithFormat:@"function %@: %@", d.name, lastError()]);
        return nil;
    }
    if (error) *error = nil;
    return function;
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d
                completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithDescriptor:d error:&error];
    handler(function, error);
}
- (NSArray<NSString *> *)functionNames
{
    NSMutableArray *names = [NSMutableArray array];
    for (uint32_t i = 0, n = nmb_library_function_count(_handle); i < n; i++) {
        char name[256];
        nmb_library_function_name(_handle, i, name, sizeof name);
        [names addObject:[NSString stringWithUTF8String:name]];
    }
    return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    const uint64_t functionBegan = nowUs();
    NmbFunction *handle = name ? nmb_function_new(_handle, name.UTF8String) : nullptr;
    gTimeFunction.add(functionBegan);
    if (!handle) return nil;
    NBFunction *function = [NBFunction new];
    function.handle = handle;
    function.name = name;
    function.functionType = (MTLFunctionType)nmb_function_type(handle);
    function.library = self;
    return (id<MTLFunction>)function;
}
// No externFunctionNames (the plugin answers it nil): 21H461's Core Image takes a graph to its legacy DAG
// (CI::LegacyDAGDescriptor, newLibraryWithImageFilterFunctionsSPI:, refused below) when its kernels are all among those names
// (CI::DAGPrecompiledKernels) but not all stitchable (CI::MetalDAGHelper::analyze_kernels); without them such a graph is made
// from source, which is served.
@end
// Each entry of a library's metallib function list, in order, until `visit` answers false: its NAME and, when both tags are there, its
// bitcode's offset into the bitcode section (OFFT's third u64) and size (MDSZ). Header: function list at 24,
// bitcode section offset and size at 72 and 80; an entry is a u32 size (counting itself), then tags (a fourcc, a u16 length and the
// bytes) up to ENDT. False when the metallib is malformed or the library kept no bytes.
static bool forEachLibraryEntry(NBLibrary *library, bool (^visit)(const std::string &, uint64_t, uint64_t, bool))
{
    const uint8_t *file = library.bytes;
    const size_t length = library.length;
    if (!file || length < 88 || memcmp(file, "MTLB", 4)) return false;
    uint64_t listAt;
    memcpy(&listAt, file + 24, 8);
    if (listAt > length - 4) return false;
    uint32_t count;
    memcpy(&count, file + listAt, 4);
    size_t at = listAt + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size;
        if (at > length - 4) return false;
        memcpy(&size, file + at, 4);
        if (size < 8 || size > length - at) return false;
        const size_t end = at + size;
        std::string name;
        uint64_t bytes = 0, offset = 0;
        bool sized = false, placed = false;
        for (size_t t = at + 4; t + 4 <= end && memcmp(file + t, "ENDT", 4);) {
            if (t + 6 > end) return false;
            uint16_t n;
            memcpy(&n, file + t + 4, 2);
            const uint8_t *v = file + t + 6;
            if (n > end - (t + 6)) return false;
            if (!memcmp(file + t, "NAME", 4))
                name.assign((const char *)v, strnlen((const char *)v, n));
            else if (!memcmp(file + t, "MDSZ", 4) && n >= 8) {
                memcpy(&bytes, v, 8);
                sized = true;
            } else if (!memcmp(file + t, "OFFT", 4) && n >= 24) {
                memcpy(&offset, v + 16, 8);
                placed = true;
            }
            t += 6 + n;
        }
        if (!visit(name, offset, bytes, sized && placed)) return true;
        at = end;
    }
    return true;
}
// A function's wrapped bitcode in its library's metallib (the entry NAMEd as the function): what a stitching request carries for each
// function, as Metal's own MTLLibraryBuilder sends it. Nil when the library kept no bytes or does not list the function.
static NSData *functionBitcode(NBFunction *function)
{
    NBLibrary *library = function.library;
    const uint8_t *file = library.bytes;
    const size_t length = library.length;
    const std::string wanted = function.name.UTF8String ?: "";
    if (!file || wanted.empty() || length < 88) return nil;
    uint64_t bitcodeAt, bitcodeSize;
    memcpy(&bitcodeAt, file + 72, 8);
    memcpy(&bitcodeSize, file + 80, 8);
    if (bitcodeAt > length || bitcodeSize > length - bitcodeAt) return nil;
    __block NSData *bitcode = nil;
    forEachLibraryEntry(library, ^(const std::string &entry, uint64_t offset, uint64_t bytes, bool placed) {
        if (entry != wanted) return true;
        if (placed && offset <= bitcodeSize && bytes <= bitcodeSize - offset)
            bitcode = [NSData dataWithBytes:file + bitcodeAt + offset length:(NSUInteger)bytes];
        return false;
    });
    return bitcode;
}
@interface NBComputePipeline : NBObject
@property (nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc
{
    nmb_compute_pipeline_release(_handle);
}
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return nmb_compute_pipeline_max_threads(_handle);
}
- (NSUInteger)threadExecutionWidth
{
    return nmb_compute_pipeline_execution_width(_handle);
}
- (NSUInteger)staticThreadgroupMemoryLength
{
    return nmb_compute_pipeline_static_threadgroup_memory(_handle);
}
@end
@interface NBRenderPipeline : NBObject
@property (nonatomic) NmbRenderPipeline *handle;
@end
@implementation NBRenderPipeline
- (void)dealloc
{
    nmb_render_pipeline_release(_handle);
}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    uint64_t s[3] = {};
    if (!nmb_device_max_threads_per_threadgroup(gDevice, s)) return 0;
    return s[0];
}
@end
@interface NBSampler : NBObject
@property (nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc
{
    nmb_sampler_release(_handle);
}
- (NSUInteger)pixelFormat
{
    return MTLPixelFormatInvalid;
}  // SPI: a sampler built from a plain descriptor names no format
@end

// Argument encoders write each argument as the 8 bytes a program could write itself (a buffer's GPU address plus offset, any other
// object's resource ID), straight into the argument buffer's mapping, so encoded and hand-written argument buffers read alike. A setter
// the runtime refuses (Metal's validation errors: a Private buffer, an index of another type) writes nothing and is logged.
@implementation NBArgumentEncoder
+ (id<MTLArgumentEncoder>)with:(NmbArgumentEncoder *)handle
{
    if (!handle) {
        NBLOG(@"native-metal: argument encoder refused: %@", lastError());
        return nil;
    }
    NBArgumentEncoder *encoder = [NBArgumentEncoder new];
    encoder.handle = handle;
    return (id<MTLArgumentEncoder>)encoder;
}
- (void)dealloc
{
    nmb_argument_encoder_release(_handle);
}
- (NSUInteger)encodedLength
{
    return nmb_argument_encoder_length(_handle);
}
- (NSUInteger)alignment
{
    return nmb_argument_encoder_alignment(_handle);
}
- (void)check:(bool)ok what:(const char *)what
{
    if (!ok) NBLOG(@"native-metal: argument encoder %s refused: %@", what, lastError());
}
- (void)setArgumentBuffer:(id<MTLBuffer>)b startOffset:(NSUInteger)o arrayElement:(NSUInteger)element
{
    if (b && ![(id)b isKindOfClass:[NBBuffer class]]) {
        NBLOG(@"native-metal: foreign argument buffer");
        return;
    }
    self.argumentBuffer = b;
    [self check:nmb_argument_encoder_set_argument_buffer(_handle, b ? [(NBBuffer *)b handle] : nullptr, o, element)
           what:"setArgumentBuffer"];
}
- (void)setArgumentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o
{
    [self setArgumentBuffer:b startOffset:o arrayElement:0];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    if (b && ![(id)b isKindOfClass:[NBBuffer class]]) {
        NBLOG(@"native-metal: foreign buffer in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_buffer(_handle, b ? [(NBBuffer *)b handle] : nullptr, o, (uint32_t)i)
           what:"setBuffer"];
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:b[k] offset:o[k] atIndex:r.location + k];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    if (t && ![(id)t isKindOfClass:[NBTexture class]]) {
        NBLOG(@"native-metal: foreign texture in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_texture(_handle, t ? [(NBTexture *)t handle] : nullptr, (uint32_t)i)
           what:"setTexture"];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self setTexture:t[k] atIndex:r.location + k];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    if (s && ![(id)s isKindOfClass:[NBSampler class]]) {
        NBLOG(@"native-metal: foreign sampler in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_sampler(_handle, s ? [(NBSampler *)s handle] : nullptr, (uint32_t)i)
           what:"setSamplerState"];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:s[k] atIndex:r.location + k];
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p atIndex:(NSUInteger)i
{
    if (p && ![(id)p isKindOfClass:[NBComputePipeline class]]) {
        NBLOG(@"native-metal: foreign pipeline in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_compute_pipeline(_handle, p ? [(NBComputePipeline *)p handle] : nullptr,
                                                          (uint32_t)i)
           what:"setComputePipelineState"];
}
- (void *)constantDataAtIndex:(NSUInteger)i
{
    void *at = nmb_argument_encoder_constant_data(_handle, (uint32_t)i);
    if (!at) NBLOG(@"native-metal: constantDataAtIndex: %@", lastError());
    return at;
}
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)i
{
    return [NBArgumentEncoder with:nmb_argument_encoder_new_nested(_handle, (uint32_t)i)];
}
@end
@interface NBDepthStencil : NBObject
@property (nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc
{
    nmb_depth_stencil_release(_handle);
}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property (nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc
{
    nmb_event_release(_handle);
}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context, uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue
{
    return nmb_shared_event_value(self.handle);
}
- (void)setSignaledValue:(uint64_t)value
{
    nmb_shared_event_set_value(self.handle, value);
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
    return nmb_shared_event_wait(self.handle, value, milliseconds);
}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener
               atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block
{
    if (!block) return;
    dispatch_queue_t queue = listener.dispatchQueue ?: dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    NSArray *job = @[ self, queue, [block copy] ];
    if (!nmb_shared_event_notify(self.handle, value, notified, (__bridge_retained void *)job)) {
        (void)(__bridge_transfer NSArray *)(__bridge void *)job;
        NBLOG(@"native-metal: notifyListener refused: %@", lastError());
    }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle
{
    if (gMissing) gMissing("NBSharedEvent", "newSharedEventHandle (cross-process)");
    return nil;
}
@end
static void notified(void *context, uint64_t value)
{
    NSArray *job = (__bridge_transfer NSArray *)context;
    id<MTLSharedEvent> event = job[0];
    MTLSharedEventNotificationBlock block = job[2];
    dispatch_async(job[1], ^{ block(event, value); });
}
@interface NBFence : NBObject
@property (nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc
{
    nmb_fence_release(_handle);
}
@end

// ---- command buffers and encoders ----
@interface NBHeap : NBObject
@property (nonatomic) NmbHeap *handle;
@property (nonatomic) MTLResourceOptions options;
@property (nonatomic) MTLHeapType heapType;
@end
@interface NBQueue : NBObject
@property (nonatomic) NmbQueue *handle;
@property (atomic, strong)
    dispatch_queue_t completionQueue;  // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end
// A render encoder's attachments as the next one compares them (joinable passes, the rates line): texture, level, slice, depth
// plane, load and store action of each color, the depth and the stencil.
struct PassTarget {
    __unsafe_unretained id texture;
    NSUInteger level, slice, plane;
    MTLLoadAction load;
    MTLStoreAction store;
};
@interface NBCommandBuffer : NBObject {
@public
    EncodedWork encodedWork;  // the command buffer's single encoder thread, not global counter deltas
    uint64_t createdUs;
    PassTarget lastPass[10];  // the previous render encoder's, while it is the last encoder opened (lastIsRender)
    bool lastIsRender, lastHadVisibility;
    NSUInteger lastWidth, lastHeight;
}
@property (nonatomic) NmbCommandBuffer *handle;
@property (nonatomic, strong) NBQueue *queue;
@property (atomic, copy) NSString *encodeFailure;
@property (atomic) BOOL committed, failedLocally;
@property (nonatomic) uint64_t committedUs;
@property (nonatomic) uint32_t commitIndex;
@property (nonatomic, strong) NSMutableArray *scheduledHandlers, *completedHandlers;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property (nonatomic, strong) NSMutableSet *resourceOwners;
@property (nonatomic, strong) dispatch_group_t handlers;
@property (nonatomic, strong) dispatch_group_t schedulingHandlers;
@property (nonatomic) BOOL schedulingDelivered;
// SPI: the dictionary MPS and Core Image keep per command buffer (MPS's ~85 call sites), and a process-unique ID for tools.
@property (nonatomic, strong, readonly) NSMutableDictionary *userDictionary;
@property (nonatomic, readonly) uint64_t globalTraceObjectID;
- (void)refuse:(NSString *)what;
@end
@interface NBEncoder : NBObject
@property (nonatomic) NmbEncoder *handle;
@property (nonatomic, strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource)
{
    const uint64_t began = nowUs();
    NSArray *pending;
    @synchronized(gInFlight) {
        pending = gInFlight.allObjects;
    }
    for (NBCommandBuffer *commandBuffer in pending) {
        int dependency = -1;
        if ([resource isKindOfClass:[NBBuffer class]])
            dependency = nmb_command_buffer_uses_buffer(commandBuffer.handle, [(NBBuffer *)resource handle]);
        else if ([resource isKindOfClass:[NBTexture class]])
            dependency = nmb_command_buffer_uses_texture(commandBuffer.handle, [(NBTexture *)resource handle]);
        gResourceChecks++;
        // An unknown result retains the old conservative wait. False means no matching storage
        // (or GPU work already completed), not a fabricated completion for a pending dependency.
        if (dependency != 0)
            [(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];
        else
            gResourceSkipped++;
    }
    gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc
{
    if (_handle) nmb_encoder_discard(_handle);
}
- (bool)check:(bool)ok what:(const char *)what
{
    if (!ok) [_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@", what, lastError()]];
    return ok;
}
- (void)endEncoding
{
    NBApiTime timed;
    NmbEncoder *handle = _handle;
    _handle = nullptr;
    if (!handle) {
        [_commandBuffer refuse:@"endEncoding twice"];
        return;
    }
    [self check:nmb_encoder_end(handle) what:"endEncoding"];
}
- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}
- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}
- (void)popDebugGroup
{}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]) {
        [_commandBuffer refuse:@"foreign buffer"];
        return;
    }
    if (buffer) [_commandBuffer.resourceOwners addObject:buffer];
    [self check:nmb_encoder_set_buffer(_handle, stage, buffer ? [(NBBuffer *)buffer handle] : nullptr, offset,
                                       (uint32_t)index)
           what:"setBuffer"];
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (texture && ![(id)texture isKindOfClass:[NBTexture class]]) {
        [_commandBuffer refuse:@"foreign texture"];
        return;
    }
    if (texture) [_commandBuffer.resourceOwners addObject:texture];
    [self check:nmb_encoder_set_texture(_handle, stage, texture ? [(NBTexture *)texture handle] : nullptr,
                                        (uint32_t)index)
           what:"setTexture"];
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (sampler && ![(id)sampler isKindOfClass:[NBSampler class]]) {
        [_commandBuffer refuse:@"foreign sampler"];
        return;
    }
    [self check:nmb_encoder_set_sampler(_handle, stage, sampler ? [(NBSampler *)sampler handle] : nullptr,
                                        (uint32_t)index)
           what:"setSamplerState"];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    [self check:nmb_encoder_set_bytes(_handle, stage, bytes, length, (uint32_t)index) what:"setBytes"];
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update
{
    if (![(id)fence isKindOfClass:[NBFence class]]) {
        [_commandBuffer refuse:@"foreign fence"];
        return;
    }
    NmbFence *handle = [(NBFence *)fence handle];
    [self check:update ? nmb_encoder_update_fence(_handle, handle, stages)
                       : nmb_encoder_wait_fence(_handle, handle, stages)
           what:update ? "updateFence" : "waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence
{
    [self fence:fence stages:0 update:true];
}
- (void)waitForFence:(id<MTLFence>)fence
{
    [self fence:fence stages:0 update:false];
}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    [self check:nmb_encoder_set_buffer_offset(_handle, stage, offset, (uint32_t)index) what:"setBufferOffset"];
}
// Resources reached through memory (argument buffers, GPU addresses): split into the bridge's buffers and textures, each kept alive by the
// command buffer like a bound one; false, with the command buffer refused, for a foreign object.
- (bool)resources:(const id __unsafe_unretained[])list
            count:(NSUInteger)count
          buffers:(std::vector<NmbBuffer *> &)buffers
         textures:(std::vector<NmbTexture *> &)textures
{
    for (NSUInteger i = 0; i < count; i++) {
        id r = list[i];
        if (!r) continue;
        if ([r isKindOfClass:[NBBuffer class]])
            buffers.push_back([(NBBuffer *)r handle]);
        else if ([r isKindOfClass:[NBTexture class]])
            textures.push_back([(NBTexture *)r handle]);
        else {
            [_commandBuffer
                refuse:[NSString stringWithFormat:@"foreign resource %s", class_getName(object_getClass(r))]];
            return false;
        }
        [_commandBuffer.resourceOwners addObject:r];
    }
    return true;
}
- (void)use:(const id __unsafe_unretained[])list
      count:(NSUInteger)count
      usage:(MTLResourceUsage)usage
     stages:(uint64_t)stages
{
    NBApiTime timed;
    std::vector<NmbBuffer *> buffers;
    std::vector<NmbTexture *> textures;
    if (![self resources:list count:count buffers:buffers textures:textures]) return;
    [self check:nmb_encoder_use_resources(_handle, buffers.data(), buffers.size(), textures.data(), textures.size(),
                                          usage, stages)
           what:"useResources"];
}
- (void)heaps:(const id __unsafe_unretained[])list count:(NSUInteger)count stages:(uint64_t)stages
{
    std::vector<NmbHeap *> heaps;
    for (NSUInteger i = 0; i < count; i++) {
        if (![list[i] isKindOfClass:[NBHeap class]]) {
            [_commandBuffer refuse:@"missing or foreign heap"];
            return;
        }
        heaps.push_back([(NBHeap *)list[i] handle]);
        [_commandBuffer.resourceOwners addObject:list[i]];
    }
    [self check:nmb_encoder_use_heaps(_handle, heaps.data(), heaps.size(), stages) what:"useHeaps"];
}
@end

@interface NBComputeEncoder : NBEncoder
@property (nonatomic) MTLDispatchType kind;
@end
@implementation NBComputeEncoder
- (MTLDispatchType)dispatchType
{
    return _kind;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    NBApiTime timed;
    if (![(id)state isKindOfClass:[NBComputePipeline class]]) {
        [self.commandBuffer refuse:@"foreign compute pipeline"];
        return;
    }
    [self check:nmb_compute_set_pipeline(self.handle, [(NBComputePipeline *)state handle])
           what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:0];
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:0];
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self buffer:b[k] offset:o[k] index:r.location + k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:0];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:0];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self texture:t[k] index:r.location + k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:0];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self check:nmb_compute_set_threadgroup_memory(self.handle, (uint32_t)n, (uint32_t)i)
           what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads
{
    NBApiTime timed;
    gDispatches++;
    self.commandBuffer->encodedWork.dispatches++;
    const uint64_t s[3] = {size.width, size.height, size.depth}, g[3] = {group.width, group.height, group.depth};
    [self check:nmb_compute_dispatch(self.handle, s, g, threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g
{
    [self dispatch:t group:g threads:true];
}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g
{
    [self dispatch:t group:g threads:false];
}
// The arguments are read when the dispatch runs (after the work before it completed): MPS's and MPSGraph's sizes computed by a kernel.
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b
                          indirectBufferOffset:(NSUInteger)o
                         threadsPerThreadgroup:(MTLSize)g
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign indirect buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    gDispatches++;
    self.commandBuffer->encodedWork.dispatches++;
    const uint64_t per[3] = {g.width, g.height, g.depth};
    [self check:nmb_compute_dispatch_indirect(self.handle, [(NBBuffer *)b handle], o, per)
           what:"dispatchThreadgroupsWithIndirectBuffer"];
}
// SPI (MTLDispatchThreadsIndirectArguments: the grid, then the threadgroup).
- (void)dispatchThreadsWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign indirect buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    gDispatches++;
    self.commandBuffer->encodedWork.dispatches++;
    [self check:nmb_compute_dispatch_indirect(self.handle, [(NBBuffer *)b handle], o, nullptr)
           what:"dispatchThreadsWithIndirectBuffer"];
}
// What a dispatch reaches through an argument buffer or a GPU address must be made resident, or its reads miss (read 0) in the runtime.
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u
{
    const id __unsafe_unretained list[1] = {r};
    [self use:list count:1 usage:u stages:0];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)n usage:(MTLResourceUsage)u
{
    [self use:r count:n usage:u stages:0];
}
- (void)useHeap:(id<MTLHeap>)h
{
    const id __unsafe_unretained list[1] = {h};
    [self heaps:list count:1 stages:0];
}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])h count:(NSUInteger)n
{
    [self heaps:h count:n stages:0];
}
// A serial encoder orders its dispatches anyway; a concurrent one (MPSGraph's) depends on these.
- (void)memoryBarrierWithScope:(MTLBarrierScope)s
{
    [self check:nmb_compute_memory_barrier(self.handle, s) what:"memoryBarrierWithScope"];
}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)n
{
    std::vector<NmbBuffer *> buffers;
    std::vector<NmbTexture *> textures;
    if (![self resources:r count:n buffers:buffers textures:textures]) return;
    [self check:nmb_compute_memory_barrier_resources(self.handle, buffers.data(), buffers.size(), textures.data(),
                                                     textures.size())
           what:"memoryBarrierWithResources"];
}
- (void)setStageInRegion:(MTLRegion)r
{
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    [self check:nmb_compute_set_stage_in_region(self.handle, region, nullptr, 0) what:"setStageInRegion"];
}
- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign stage-in buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    [self check:nmb_compute_set_stage_in_region(self.handle, nullptr, [(NBBuffer *)b handle], o)
           what:"setStageInRegionWithIndirectBuffer"];
}
// Imageblocks are an Apple4 feature this device does not claim; the runtime keeps Metal's sizing rules for a kernel that has one.
- (void)setImageblockWidth:(NSUInteger)w height:(NSUInteger)h
{
    [self check:nmb_compute_set_imageblock_size(self.handle, w, h) what:"setImageblockWidth"];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self sampler:s[k] index:r.location + k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i
{
    if (s && ![(id)s isKindOfClass:[NBSampler class]]) {
        [self.commandBuffer refuse:@"foreign sampler"];
        return;
    }
    [self check:nmb_compute_set_sampler_lod(self.handle, s ? [(NBSampler *)s handle] : nullptr, lo, hi, (uint32_t)i)
           what:"setSamplerState:lodMinClamp:lodMaxClamp:"];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
            lodMinClamps:(const float[])lo
            lodMaxClamps:(const float[])hi
               withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location + k];
}
@end

@interface NBRenderEncoder : NBEncoder
@end
@implementation NBRenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    NBApiTime timed;
    if (![(id)state isKindOfClass:[NBRenderPipeline class]]) {
        [self.commandBuffer refuse:@"foreign render pipeline"];
        return;
    }
    [self check:nmb_render_set_pipeline(self.handle, [(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:1];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:2];
}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:1];
}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:2];
}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self buffer:b[k] offset:o[k] index:r.location + k stage:1];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self buffer:b[k] offset:o[k] index:r.location + k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:1];
}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:2];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:1];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:2];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self texture:t[k] index:r.location + k stage:2];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:1];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:2];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
    NBApiTime timed;
    if (state && ![(id)state isKindOfClass:[NBDepthStencil class]]) {
        [self.commandBuffer refuse:@"foreign depth/stencil state"];
        return;
    }
    [self check:nmb_render_set_depth_stencil(self.handle, state ? [(NBDepthStencil *)state handle] : nullptr)
           what:"setDepthStencilState"];
}
- (void)setViewport:(MTLViewport)v
{
    NBApiTime timed;
    const double values[6] = {v.originX, v.originY, v.width, v.height, v.znear, v.zfar};
    [self check:nmb_render_set_viewport(self.handle, values) what:"setViewport"];
}
- (void)setScissorRect:(MTLScissorRect)r
{
    NBApiTime timed;
    const uint64_t values[4] = {r.x, r.y, r.width, r.height};
    [self check:nmb_render_set_scissor(self.handle, values) what:"setScissorRect"];
}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults until each is set.
- (void)raster
{
    NBApiTime timed;
    NSNumber *cull = objc_getAssociatedObject(self, "cull"), *winding = objc_getAssociatedObject(self, "winding"),
             *fill = objc_getAssociatedObject(self, "fill");
    [self check:nmb_render_set_raster(self.handle, cull.unsignedLongLongValue, winding.unsignedLongLongValue,
                                      fill.unsignedLongLongValue)
           what:"raster state"];
}
- (void)setCullMode:(MTLCullMode)m
{
    objc_setAssociatedObject(self, "cull", @(m), OBJC_ASSOCIATION_RETAIN);
    [self raster];
}
- (void)setFrontFacingWinding:(MTLWinding)w
{
    objc_setAssociatedObject(self, "winding", @(w), OBJC_ASSOCIATION_RETAIN);
    [self raster];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f
{
    objc_setAssociatedObject(self, "fill", @(f), OBJC_ASSOCIATION_RETAIN);
    [self raster];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp
{
    [self check:nmb_render_set_depth_bias(self.handle, bias, slope, clamp) what:"setDepthBias"];
}
- (void)setLineWidth:(float)width
{
    [self check:nmb_render_set_line_width(self.handle, width) what:"setLineWidth"];
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    [self check:nmb_render_set_visibility_mode(self.handle, mode, offset) what:"setVisibilityResultMode"];
}
- (void)setFragmentTexture:(id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    [self texture:t index:ti stage:2];
    [self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth
{
    return 0;
}
- (NSUInteger)tileHeight
{
    return 0;
}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    (void)t;
    (void)i;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    (void)p;
    (void)n;
    (void)i;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
    (void)size;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region
{
    (void)size;
    (void)region;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}
- (void)setStencilReferenceValue:(uint32_t)v
{
    [self check:nmb_render_set_stencil_reference(self.handle, v, v) what:"setStencilReferenceValue"];
}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b
{
    [self check:nmb_render_set_stencil_reference(self.handle, f, b) what:"setStencilReferenceValues"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
    const float c[4] = {r, g, b, a};
    [self check:nmb_render_set_blend_color(self.handle, c) what:"setBlendColor"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t
           vertexStart:(NSUInteger)s
           vertexCount:(NSUInteger)n
         instanceCount:(NSUInteger)i
          baseInstance:(NSUInteger)b
{
    NBApiTime timed;
    gDraws++;
    self.commandBuffer->encodedWork.draws++;
    [self check:nmb_render_draw(self.handle, t, (uint32_t)s, (uint32_t)n, (uint32_t)i, (uint32_t)b)
           what:"drawPrimitives"];
}
- (void)drawPrimitives:(MTLPrimitiveType)t
           vertexStart:(NSUInteger)s
           vertexCount:(NSUInteger)n
         instanceCount:(NSUInteger)i
{
    [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
{
    [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i
                   baseVertex:(NSInteger)v
                 baseInstance:(NSUInteger)bi
{
    NBApiTime timed;
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign index buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    gDraws++;
    self.commandBuffer->encodedWork.draws++;
    [self check:nmb_render_draw_indexed(self.handle, t, (uint32_t)n, k, [(NBBuffer *)b handle], o, (uint32_t)i,
                                        (int32_t)v, (uint32_t)bi)
           what:"drawIndexedPrimitives"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i
{
    [self drawIndexedPrimitives:t
                     indexCount:n
                      indexType:k
                    indexBuffer:b
              indexBufferOffset:o
                  instanceCount:i
                     baseVertex:0
                   baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
{
    [self drawIndexedPrimitives:t
                     indexCount:n
                      indexType:k
                    indexBuffer:b
              indexBufferOffset:o
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s
{
    const id __unsafe_unretained list[1] = {r};
    [self use:list count:1 usage:u stages:s];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r
               count:(NSUInteger)n
               usage:(MTLResourceUsage)u
              stages:(MTLRenderStages)s
{
    [self use:r count:n usage:u stages:s];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u
{
    [self useResource:r usage:u stages:MTLRenderStageVertex | MTLRenderStageFragment];
}
- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)s
{
    const id __unsafe_unretained list[1] = {h};
    [self heaps:list count:1 stages:s];
}
- (void)useHeap:(id<MTLHeap>)h
{
    [self useHeap:h stages:MTLRenderStageVertex | MTLRenderStageFragment];
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    [self fence:fence stages:stages update:true];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    [self fence:fence stages:stages update:false];
}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count
{
    for (size_t i = 0; i < count; i++)
        if (![objects[i] isKindOfClass:[NBBuffer class]]) {
            [self.commandBuffer refuse:@"missing or foreign blit buffer"];
            return false;
        }
    for (size_t i = 0; i < count; i++) [self.commandBuffer.resourceOwners addObject:objects[i]];
    return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count
{
    for (size_t i = 0; i < count; i++) {
        id o = objects[i];
        if (![o isKindOfClass:[NBTexture class]]) {
            [self.commandBuffer refuse:@"missing or foreign blit texture"];
            return false;
        }
        [self.commandBuffer.resourceOwners addObject:o];
    }
    return true;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dO
                  size:(NSUInteger)n
{
    NBApiTime timed;
    if (![self buffers:NB_OBJECTS(s, d) count:2]) return;
    [self check:nmb_blit_copy_buffer(self.handle, [(NBBuffer *)s handle], so, [(NBBuffer *)d handle], dO, n)
           what:"copyFromBuffer:toBuffer:"];
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v
{
    if (![self buffers:NB_OBJECTS(b) count:1]) return;
    [self check:nmb_blit_fill_buffer(self.handle, [(NBBuffer *)b handle], r.location, r.location + r.length, v)
           what:"fillBuffer"];
}
- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dO
{
    NBApiTime timed;
    if (![self textures:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1]) return;
    const uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {z.width, z.height, z.depth}, b[3] = {dO.x, dO.y, dO.z};
    [self check:nmb_blit_copy_texture(self.handle, [(NBTexture *)s handle], ss, sl, a, size, [(NBTexture *)d handle],
                                      ds, dl, b)
           what:"copyFromTexture:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    if (![self textures:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1]) return;
    const NSUInteger levels = MIN(s.mipmapLevelCount, d.mipmapLevelCount), slices = MIN(s.arrayLength, d.arrayLength);
    for (NSUInteger slice = 0; slice < slices; slice++)
        for (NSUInteger level = 0; level < levels; level++) {
            MTLSize size = MTLSizeMake(MAX(s.width >> level, (NSUInteger)1), MAX(s.height >> level, (NSUInteger)1),
                                       MAX(s.depth >> level, (NSUInteger)1));
            [self copyFromTexture:s
                      sourceSlice:slice
                      sourceLevel:level
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:size
                        toTexture:d
                 destinationSlice:slice
                 destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)o
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)slice
       destinationLevel:(NSUInteger)level
      destinationOrigin:(MTLOrigin)dO
{
    NBApiTime timed;
    if (![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1]) return;
    const uint64_t size[3] = {z.width, z.height, z.depth}, b[3] = {dO.x, dO.y, dO.z};
    [self check:nmb_blit_buffer_to_texture(self.handle, [(NBBuffer *)s handle], o, row, image, size,
                                           [(NBTexture *)d handle], slice, level, b)
           what:"copyFromBuffer:toTexture:"];
}
- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)slice
                 sourceLevel:(NSUInteger)level
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)z
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)o
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    NBApiTime timed;
    if (![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1]) return;
    const uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {z.width, z.height, z.depth};
    [self check:nmb_blit_texture_to_buffer(self.handle, [(NBTexture *)s handle], slice, level, a, size,
                                           [(NBBuffer *)d handle], o, row, image)
           what:"copyFromTexture:toBuffer:"];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    if (![self textures:NB_OBJECTS(t) count:1]) return;
    [self check:nmb_blit_generate_mipmaps(self.handle, [(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}
- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;
}
@end

static void completed(void *context);
static void scheduled(void *context);
@implementation NBCommandBuffer
- (instancetype)init
{
    static std::atomic<uint64_t> traceIDs{0};
    if ((self = [super init])) {
        _userDictionary = [NSMutableDictionary dictionary];
        _globalTraceObjectID = ++traceIDs;
        _scheduledHandlers = [NSMutableArray array];
        _completedHandlers = [NSMutableArray array];
        _resourceOwners = [NSMutableSet set];
        _handlers = dispatch_group_create();
        _schedulingHandlers = dispatch_group_create();
    }
    if (self) createdUs = nowUs();
    return self;
}
- (void)dealloc
{
    nmb_command_buffer_release(_handle);
}
- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}
- (BOOL)retainedReferences
{
    return YES;
}
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions
{
    return 0;
}
- (void)setProtectionOptions:(uint64_t)options
{
    if (options) [self refuse:[NSString stringWithFormat:@"protection options 0x%llx", options]];
}
- (void)refuse:(NSString *)what
{
    if (!self.encodeFailure) {
        self.encodeFailure = what;
        NBLOG(@"native-metal: command buffer refused: %@", what);
    }
}
- (MTLCommandBufferStatus)status
{
    if (self.failedLocally) return MTLCommandBufferStatusError;
    return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error
{
    if (self.failedLocally) return nbError(self.encodeFailure);
    if (self.status != MTLCommandBufferStatusError) return nil;
    char text[512] = {};
    nmb_command_buffer_error(_handle, text, sizeof text);
    return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name
{
    (void)name;
}
- (void)popDebugGroup
{}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained[])groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}
- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained[])groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime
{
    return 0;
}
- (CFTimeInterval)kernelEndTime
{
    return 0;
}
- (BOOL)commitAndWaitUntilSubmitted
{
    [(id<MTLCommandBuffer>)self commit];
    [self waitUntilScheduled];
    return !self.failedLocally;
}
- (CFTimeInterval)GPUStartTime
{
    return nmb_command_buffer_gpu_time(_handle, false);
}
- (CFTimeInterval)GPUEndTime
{
    return nmb_command_buffer_gpu_time(_handle, true);
}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what
{
    if (self.committed) {
        [self refuse:[what stringByAppendingString:@" after commit"]];
        return nil;
    }
    if (!handle) {
        [self refuse:[NSString stringWithFormat:@"%@: %@", what, lastError()]];
        return nil;
    }
    encoder.handle = handle;
    encoder.commandBuffer = self;
    return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}
// A concurrent encoder leaves its dispatches' order to its memory barriers, which the runtime records.
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    NBApiTime timed;
    lastIsRender = false;
    NBComputeEncoder *encoder = [NBComputeEncoder new];
    encoder.kind = type;
    return [self encoder:encoder
                  handle:self.committed ? nullptr : nmb_compute_encoder_new_with_dispatch_type(_handle, type)
                    what:@"computeCommandEncoder"];
}
// The pass descriptor's counter sample buffers are not offered (no counter sets): one that names any fails the command buffer.
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    for (NSUInteger i = 0; i < 4; i++)
        if (d.sampleBufferAttachments[i].sampleBuffer) {
            [self refuse:@"compute pass counter samples"];
            return nil;
        }
    return [self computeCommandEncoderWithDispatchType:d.dispatchType];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    NBApiTime timed;
    lastIsRender = false;
    gBlits++;
    encodedWork.blits++;
    return [self encoder:[NBBlitEncoder new]
                  handle:self.committed ? nullptr : nmb_blit_encoder_new(_handle)
                    what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    NBApiTime timed;
    NmbRenderPassDescriptor pass = {};
    {
        id<MTLTexture> first = d.colorAttachments[0].texture;
        if (first) {
            const uint64_t pixels = (uint64_t)first.width * first.height;
            gPasses++;
            gPassPixels += pixels;
            encodedWork.passes++;
            encodedWork.pixels += pixels;
            uint64_t seen = gBiggestPass.load();
            while (pixels > seen && !gBiggestPass.compare_exchange_weak(seen, pixels)) {}
            if (pixels >= 2000000) gFullScreenPasses++;
        }
    }
    auto attachment = [&](MTLRenderPassAttachmentDescriptor *a, NmbPassAttachment &out) -> bool {
        if (!a.texture) return true;
        if (![(id)a.texture isKindOfClass:[NBTexture class]]) return false;
        NBTexture *texture = (NBTexture *)a.texture;
        [_resourceOwners addObject:texture];
        out.texture = texture.handle;
        out.level = a.level;
        out.slice = a.slice;
        out.depth_plane = a.depthPlane;
        out.load = a.loadAction;
        out.store = a.storeAction;
        return true;
    };
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!attachment(c, pass.colors[i])) {
            [self refuse:@"foreign color attachment"];
            return nil;
        }
        pass.colors[i].clear[0] = c.clearColor.red;
        pass.colors[i].clear[1] = c.clearColor.green;
        pass.colors[i].clear[2] = c.clearColor.blue;
        pass.colors[i].clear[3] = c.clearColor.alpha;
    }
    if (!attachment(d.depthAttachment, pass.depth) || !attachment(d.stencilAttachment, pass.stencil)) {
        [self refuse:@"foreign depth/stencil attachment"];
        return nil;
    }
    pass.depth.clear[0] = d.depthAttachment.clearDepth;
    pass.stencil.clear[0] = d.stencilAttachment.clearStencil;
    pass.width = d.renderTargetWidth;
    pass.height = d.renderTargetHeight;
    pass.array_length = d.renderTargetArrayLength;
    pass.samples = d.defaultRasterSampleCount;
    if (d.visibilityResultBuffer) {
        if (![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]) {
            [self refuse:@"foreign visibility result buffer"];
            return nil;
        }
        [_resourceOwners addObject:d.visibilityResultBuffer];
        pass.visibility = [(NBBuffer *)d.visibilityResultBuffer handle];
    }
    [self notePass:d];
    return [self encoder:[NBRenderEncoder new]
                  handle:self.committed ? nullptr : nmb_render_encoder_new(_handle, &pass)
                    what:@"renderCommandEncoder"];
}
// Count a render encoder that opens right after another of this command buffer on exactly its attachments, loading each it stored
// (DontCare after DontCare for one it discarded), with no visibility results: two passes one pass could hold.
- (void)notePass:(MTLRenderPassDescriptor *)d
{
    PassTarget now[10];
    for (unsigned i = 0; i < 10; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < 8    ? (MTLRenderPassAttachmentDescriptor *)d.colorAttachments[i]
                                               : i == 8 ? (MTLRenderPassAttachmentDescriptor *)d.depthAttachment
                                                        : (MTLRenderPassAttachmentDescriptor *)d.stencilAttachment;
        id texture = a.texture;
        now[i] = texture ? PassTarget{texture, a.level, a.slice, a.depthPlane, a.loadAction, a.storeAction}
                         : PassTarget{nil, 0, 0, 0, MTLLoadActionDontCare, MTLStoreActionDontCare};
    }
    const bool visibility = d.visibilityResultBuffer != nil;
    if (lastIsRender && !lastHadVisibility && !visibility && lastWidth == d.renderTargetWidth &&
        lastHeight == d.renderTargetHeight) {
        bool joinable = true;
        for (unsigned i = 0; i < 10 && joinable; i++) {
            const PassTarget &was = lastPass[i], &is = now[i];
            if (was.texture != is.texture) {
                joinable = false;
                break;
            }
            if (!is.texture) continue;
            joinable = was.level == is.level && was.slice == is.slice && was.plane == is.plane &&
                       (is.load == MTLLoadActionLoad ||
                        (was.store == MTLStoreActionDontCare && is.load == MTLLoadActionDontCare));
        }
        if (joinable) gJoinablePasses++;
    }
    memcpy(lastPass, now, sizeof now);
    lastIsRender = true;
    lastHadVisibility = visibility;
    lastWidth = d.renderTargetWidth;
    lastHeight = d.renderTargetHeight;
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (block) @synchronized(self) {
            [_scheduledHandlers addObject:[block copy]];
        }
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (block) @synchronized(self) {
            [_completedHandlers addObject:[block copy]];
        }
}
- (void)enqueue
{}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal
{
    lastIsRender = false;
    if (![(id)event isKindOfClass:[NBEvent class]]) {
        [self refuse:@"foreign event"];
        return;
    }
    if (self.committed) {
        [self refuse:@"event encoded after commit"];
        return;
    }
    NmbEvent *handle = [(NBEvent *)event handle];
    if (!(signal ? nmb_command_buffer_signal_event(_handle, handle, value)
                 : nmb_command_buffer_wait_event(_handle, handle, value)))
        [self refuse:[NSString
                         stringWithFormat:@"%s: %@", signal ? "encodeSignalEvent" : "encodeWaitForEvent", lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false];
}
// IOGPU SPI (MPSGraph): signal once the command buffer is scheduled. Signalled at its place in the command stream instead, which is
// never earlier than scheduling: a waiter only waits longer.
- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}
// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule
{
    {
        const uint64_t took = nowUs() - self.committedUs;
        std::lock_guard<std::mutex> lock(gCommitLock);
        CommitRecord &r = gCommitRing[self.commitIndex % 32];
        if (r.index == self.commitIndex) r.scheduledUs = took;
    }
    NSArray *handlers;
    @synchronized(self) {
        _schedulingDelivered = YES;
        handlers = [_scheduledHandlers copy];
        [_scheduledHandlers removeAllObjects];
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler block in handlers) block((id<MTLCommandBuffer>)self);
        dispatch_group_leave(self->_schedulingHandlers);
    };
    // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
    // deadlock checks. The queue's completionQueue routes completed handlers only.
    run();
}
// After completion (or a refusal): completed handlers, once.
- (void)finish
{
    // The GPU finished (or a local refusal never submitted). Mark every host import this command buffer used GPU-dirty: a cheap
    // flag store, no cache work here, so the render and completion paths pay nothing. The lines are invalidated lazily, only if
    // and when this process reads the import back (NBBuffer contents / NBTexture getBytes). Over-marking a read-only import is
    // harmless: its one later readback (rare) invalidates once. The framebuffer, which the CPU never reads, costs only the flag.
    if (!self.failedLocally)
        for (id owner in _resourceOwners) {
            NBBuffer *import = hostImportOf(owner);
            if (import) [import gpuWrote];
        }
    // Actual GPU completion (or a local refusal that never submitted) is the
    // release point. A retained completed command must not pin every old frame.
    [_resourceOwners removeAllObjects];
    @synchronized(gInFlight) {
        [gInFlight removeObject:self];
    }
    NSArray *completedHandlers;
    @synchronized(self) {
        // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
        if (!_schedulingDelivered) {
            _schedulingDelivered = YES;
            [_scheduledHandlers removeAllObjects];
            dispatch_group_leave(_schedulingHandlers);
        }
        completedHandlers = [_completedHandlers copy];
        [_completedHandlers removeAllObjects];
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler block in completedHandlers) block((id<MTLCommandBuffer>)self);
        dispatch_group_leave(self->_handlers);
    };
    dispatch_queue_t queue = _queue.completionQueue;
    if (queue)
        dispatch_async(queue, run);
    else
        run();
}
- (void)commit
{
    const uint64_t began = apiTiming() ? mach_absolute_time() : 0;
    [self commitNow];
    if (began) {
        gApiUs += ticksToUs(tApiTicks + mach_absolute_time() - began);
        tApiTicks = 0;
    }
}
- (void)commitNow
{
    if (self.committed) {
        NBLOG(@"native-metal: commit twice");
        return;
    }
    self.committed = YES;
    self.committedUs = nowUs();
    {
        std::lock_guard<std::mutex> lock(gCommitLock);
        // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
        const uint32_t index = (uint32_t)(gCommits.fetch_add(1) + 1);
        self.commitIndex = index;
        CommitRecord &r = gCommitRing[index % 32];
        r = {};
        r.index = index;
        r.committedUs = self.committedUs;
        r.ageUs = self.committedUs - createdUs;
        r.work = encodedWork;
    }
    gEncodeUs += self.committedUs - createdUs;
    gHistEncode.add(self.committedUs - createdUs);
    if (tLastCommitUs && self.committedUs > tLastCommitUs && self.committedUs - tLastCommitUs < 250000)
        gHistGap.add(self.committedUs - tLastCommitUs);
    tLastCommitUs = self.committedUs;
    dispatch_group_enter(_handlers);
    dispatch_group_enter(_schedulingHandlers);
    @synchronized(gInFlight) {
        [gInFlight addObject:self];
    }
    if (!self.encodeFailure) {
        // No cache maintenance on the submit path: CPU writes into imports reach the GPU through the kernel's IOSurface-unlock
        // clean (the writer's unlock, in any process), and the import-time clean (BoImport). The render path pays nothing.
        // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
        void *context = (__bridge_retained void *)self;
        bool committed = nmb_command_buffer_on_completed(_handle, completed, context) &&
                         nmb_command_buffer_on_scheduled(_handle, scheduled, context);
        if (committed) {
            const uint64_t began = nowUs();
            committed = nmb_command_buffer_commit(_handle);
            gTimeSubmit.add(began);
        }
        if (committed) return;
        (void)CFBridgingRelease(context);  // refused registration/commit cannot call the registered handlers
        [self refuse:[@"commit: " stringByAppendingString:lastError()]];
    }
    self.failedLocally = YES;
    gCommitFailures++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ [self finish]; });
}
- (void)waitUntilCompleted
{
    if (!self.committed) return;
    const uint64_t began = nowUs();
    if (!self.failedLocally) nmb_command_buffer_wait(_handle, 0);
    dispatch_group_wait(_handlers, DISPATCH_TIME_FOREVER);
    gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled
{
    if (!self.committed) return;
    const uint64_t began = nowUs();
    if (!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)) {
        NBLOG(@"native-metal: waitUntilScheduled refused: %@", lastError());
        return;
    }
    dispatch_group_wait(_schedulingHandlers, DISPATCH_TIME_FOREVER);
    gTimeWaitScheduled.add(began);
    gHistSched.add(nowUs() - began);
}
@end
static void scheduled(void *context)
{
    [(__bridge NBCommandBuffer *)context didSchedule];
}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context)
{
    gCompleted++;
    NBCommandBuffer *commandBuffer = (__bridge_transfer NBCommandBuffer *)context;
    {
        const uint64_t took = nowUs() - commandBuffer.committedUs;
        gTimeEncodeToCommit.count++;
        gTimeEncodeToCommit.total += took;
        gHistGpu.add(took);
        uint64_t seen = gTimeEncodeToCommit.slowest.load();
        while (took > seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen, took)) {}
        std::lock_guard<std::mutex> lock(gCommitLock);
        CommitRecord &r = gCommitRing[commandBuffer.commitIndex % 32];
        if (r.index == commandBuffer.commitIndex) r.callbackUs = took;
    }
    [commandBuffer finish];
}

@implementation NBQueue
- (void)dealloc
{
    nmb_queue_release(_handle);
}
- (id<MTLCommandBuffer>)commandBuffer
{
    NBApiTime timed;
    NmbCommandBuffer *handle = nmb_command_buffer_new(_handle);
    if (!handle) {
        NBLOG(@"native-metal: commandBuffer refused: %@", lastError());
        return nil;
    }
    NBCommandBuffer *commandBuffer = [NBCommandBuffer new];
    commandBuffer.handle = handle;
    commandBuffer.queue = self;
    return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];
}
- (void)setSubmissionQueue:(dispatch_queue_t)queue
{
    (void)queue;
}  // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// GPU priorities are not applied (one hardware queue): reported as not applied.
- (BOOL)setGPUPriority:(NSUInteger)priority
{
    (void)priority;
    return NO;
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
    (void)priority;
    return NO;
}
- (NSUInteger)getGPUPriority
{
    return 0;
}  // none was applied (above)
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error, NSString *what, T value)
{
    noteProblem([NSString stringWithFormat:@"%@: %@", what, lastError()]);
    if (error) *error = nbError([NSString stringWithFormat:@"%@: %@", what, lastError()]);
    return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers()
{
    void *frames[24];
    const int n = backtrace(frames, 24);
    std::string out = "qc";
    char item[32];
    unsigned kept = 0;
    uintptr_t header = 0;
    for (int i = 0; i < n && kept < 8; i++) {
        Dl_info info{};
        if (!dladdr(frames[i], &info) || !info.dli_fname || !strstr(info.dli_fname, "/QuartzCore")) continue;
        header = (uintptr_t)info.dli_fbase;
        snprintf(item, sizeof item, " %lx", (unsigned long)((uintptr_t)frames[i] - header));
        out += item;
        ++kept;
    }
    // The context's own flags (int-154: the draws never took the generic path, which only bit 18 of MetalContext +0x1a0 or the debug byte
    // can cause, and the debug byte was 0). MetalContext::create_pipeline_state (header +0x143000) saves its caller's x19 at fp-8, and its
    // caller MetalContext::draw keeps `this` in x19: the frame record whose return address is +0x143214 links to that frame. Read with
    // vm_read so a different stack layout reports ctx=? instead of faulting.
    if (header) {
        uintptr_t fp = (uintptr_t)__builtin_frame_address(0), ctx = 0;
        for (unsigned depth = 0; fp && depth < 24; ++depth) {
            uintptr_t record[2] = {};
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), fp, sizeof record, (vm_address_t)record, &got) != KERN_SUCCESS ||
                got != sizeof record)
                break;
            if (record[1] - header == 0x143214) {
                uintptr_t saved = 0;
                if (record[0] && vm_read_overwrite(mach_task_self(), record[0] - 8, sizeof saved, (vm_address_t)&saved,
                                                   &got) == KERN_SUCCESS)
                    ctx = saved;
                break;
            }
            if (record[0] <= fp) break;
            fp = record[0];
        }
        uint32_t flags[2] = {};
        vm_size_t got = 0;
        if (ctx && vm_read_overwrite(mach_task_self(), ctx + 0x1a0, 4, (vm_address_t)&flags[0], &got) == KERN_SUCCESS &&
            vm_read_overwrite(mach_task_self(), ctx + 0xb3c, 4, (vm_address_t)&flags[1], &got) == KERN_SUCCESS) {
            snprintf(item, sizeof item, " ctx=%x/%x", flags[0], flags[1]);
            out += item;
        } else
            out += " ctx=?";
    }
    static auto global = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "CARenderGetGlobalFeatureFlags");
    snprintf(item, sizeof item, " global=%x", global ? global() : 0xffffffffu);
    out += item;
    if (header) {
        const uintptr_t options = header + (0x1f491b3c5ull - 0x189429000ull);
        Dl_info info{};
        if (dladdr((void *)options, &info) && info.dli_fname && strstr(info.dli_fname, "/QuartzCore") &&
            (uintptr_t)info.dli_fbase == header) {
            const volatile uint8_t *b = (const volatile uint8_t *)options;
            snprintf(item, sizeof item, " dbg=%u%u%u", b[0], b[1], b[2]);
            out += item;
        } else
            out += " dbg=?";
    }
    return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
    uint64_t user = 0, system = 0;
    int64_t faults = 0, pageins = 0, switches = 0;
    static NBCost now()
    {
        NBCost c;
        thread_basic_info_data_t t{};
        mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&t, &n) == KERN_SUCCESS) {
            c.user = (uint64_t)t.user_time.seconds * 1000000 + t.user_time.microseconds;
            c.system = (uint64_t)t.system_time.seconds * 1000000 + t.system_time.microseconds;
        }
        mach_port_deallocate(mach_task_self(), self);
        task_events_info_data_t e{};
        n = TASK_EVENTS_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&e, &n) == KERN_SUCCESS) {
            c.faults = e.faults;
            c.pageins = e.pageins;
            c.switches = e.csw;
        }
        return c;
    }
    std::string since(const NBCost &b) const
    {
        char text[128];
        snprintf(text, sizeof text, " cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",
                 (unsigned long long)(user + system - b.user - b.system), (unsigned long long)(system - b.system),
                 (long long)(faults - b.faults), (long long)(pageins - b.pageins), (long long)(switches - b.switches));
        return text;
    }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d, uint64_t began, const char *refused,
                                  const std::string &cost)
{
    if (gJournalPath.empty()) return;
    const uint64_t ended = nowUs();
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    char head[256];
    snprintf(head, sizeof head, "R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",
             (unsigned long long)((began - gJournalBegan) / 1000), (unsigned long long)tid, pthread_main_np(),
             (unsigned long long)(ended - began), refused ? 0 : 1, (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string line = head;
    line += cost;
    line += " label=";
    line += d.label.length ? d.label.UTF8String : "-";
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid) continue;
        snprintf(head, sizeof head, " c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        line += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            snprintf(head, sizeof head, " a%u=%lu:%lu:%lu:%lu:%lu:%lu", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, (unsigned long)l.stride,
                     (unsigned long)l.stepFunction, (unsigned long)l.stepRate);
            line += head;
        }
    NBFunction *vf = [(id)d.vertexFunction isKindOfClass:[NBFunction class]] ? (NBFunction *)d.vertexFunction : nil;
    NBFunction *ff = [(id)d.fragmentFunction isKindOfClass:[NBFunction class]] ? (NBFunction *)d.fragmentFunction : nil;
    line += " | v ";
    line += vf ? vf.name.UTF8String : "-";
    line += " ";
    line += vf ? vf.constantsText : "";
    line += " | f ";
    line += ff ? ff.name.UTF8String : "-";
    line += " ";
    line += ff ? ff.constantsText : "";
    if (refused) {
        line += " | refused ";
        line += refused;
    }
    line += " | ";
    line += quartzCoreCallers();
    for (char &c : line)
        if (c == '\n') c = ' ';
    journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d, NmbRenderPipelineDescriptor *p, NSError **error)
{
    if (![(id)d.vertexFunction isKindOfClass:[NBFunction class]] ||
        (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
        if (error) *error = nbError(@"foreign function");
        return false;
    }
    p->vertex = [(NBFunction *)d.vertexFunction handle];
    p->fragment = d.fragmentFunction ? [(NBFunction *)d.fragmentFunction handle] : nullptr;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        p->colors[i] = {(uint64_t)a.pixelFormat,
                        (uint64_t)a.writeMask,
                        (uint64_t)a.blendingEnabled,
                        (uint64_t)a.sourceRGBBlendFactor,
                        (uint64_t)a.destinationRGBBlendFactor,
                        (uint64_t)a.rgbBlendOperation,
                        (uint64_t)a.sourceAlphaBlendFactor,
                        (uint64_t)a.destinationAlphaBlendFactor,
                        (uint64_t)a.alphaBlendOperation};
    }
    p->depth_format = d.depthAttachmentPixelFormat;
    p->stencil_format = d.stencilAttachmentPixelFormat;
    p->samples = d.rasterSampleCount;
    p->topology = d.inputPrimitiveTopology;
    p->rasterization = d.rasterizationEnabled;
    if (MTLVertexDescriptor *v = d.vertexDescriptor) {
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            p->vertex_inputs = 1;
            p->attributes[i] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex];
                p->layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
    }
    return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode. (Its interface is
// with the encoders', which make heaps resident.)
@implementation NBHeap
- (void)dealloc
{
    nmb_heap_release(_handle);
}
- (NSUInteger)size
{
    return nmb_heap_size(_handle);
}
- (NSUInteger)usedSize
{
    return nmb_heap_used_size(_handle);
}
- (NSUInteger)currentAllocatedSize
{
    return nmb_heap_allocated_size(_handle);
}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    return nmb_heap_max_available_size(_handle, alignment);
}
- (MTLResourceOptions)resourceOptions
{
    return _options;
}
- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return (MTLHazardTrackingMode)((_options & MTLResourceHazardTrackingModeMask) >>
                                   MTLResourceHazardTrackingModeShift);
}
- (MTLHeapType)type
{
    return _heapType;
}
- (uint64_t)protectionOptions
{
    return 0;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_heap_set_purgeable_state(_handle, state);
    if (!answer) NBLOG(@"native-metal: heap setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset
{
    NmbBuffer *handle = nmb_heap_new_buffer(_handle, length, options, offset);
    if (!handle) {
        NBLOG(@"native-metal: heap buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    buffer.parentHeap = (id<MTLHeap>)self;
    buffer.heapOffset = offset == UINT64_MAX ? 0 : offset;
    return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    return [self bufferWithLength:length options:options offset:UINT64_MAX];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor)) return nil;
    NmbTexture *handle = nmb_heap_new_texture(_handle, &descriptor, offset);
    if (!handle) {
        NBLOG(@"native-metal: heap texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.parentHeap = (id<MTLHeap>)self;
    texture.heapOffset = offset == UINT64_MAX ? 0 : offset;
    return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [self textureWithDescriptor:d offset:UINT64_MAX];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    return [self textureWithDescriptor:d offset:offset];
}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property (nonatomic, strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
static bool linkedHandles(NSArray *functions, std::vector<NmbFunction *> &out)
{
    for (id f in functions) {
        if (![f isKindOfClass:[NBFunction class]]) return false;
        out.push_back([(NBFunction *)f handle]);
    }
    return true;
}
// `linked` holds the linked functions' handles (functions, then privateFunctions) that `p` points into: it outlives the creation call.
static bool computeDescriptor(MTLComputePipelineDescriptor *d, NmbComputePipelineDescriptor *p,
                              std::vector<NmbFunction *> &linked, NSError **error)
{
    if (!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]) {
        if (error) *error = nbError(@"foreign compute function");
        return false;
    }
    // Metal may materialize empty stage/linked descriptors on access. Preserve the
    // default meaning while refusing state this frontend cannot yet represent.
    MTLStageInputOutputDescriptor *stage = d.stageInputDescriptor;
    bool empty = !stage || (stage.indexType == MTLIndexTypeUInt16 && stage.indexBufferIndex == 0);
    for (NSUInteger index = 0; stage && index < 31; ++index) {
        MTLAttributeDescriptor *a = stage.attributes[index];
        MTLBufferLayoutDescriptor *b = stage.layouts[index];
        empty = empty && a.format == MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
                b.stepFunction == MTLStepFunctionPerVertex && b.stepRate == 1;
    }
    if (!empty) {
        if (error) *error = nbError(@"compute stage inputs are not implemented");
        return false;
    }
    // Visible functions the kernel calls by name (MPS's ND-array kernels import their prologue and epilogue, prefixPrimary_f and the like,
    // through these); binary functions, call-site groups and dynamic libraries are not represented yet.
    MTLLinkedFunctions *functions = d.linkedFunctions;
    if (functions.binaryFunctions.count || functions.groups.count || d.preloadedLibraries.count ||
        d.insertLibraries.count) {
        if (error)
            *error = nbError(@"compute binary functions, function groups and dynamic libraries are not implemented");
        return false;
    }
    linked.clear();
    if (!linkedHandles(functions.functions, linked)) {
        if (error) *error = nbError(@"foreign linked function");
        return false;
    }
    const size_t visible = linked.size();
    if (!linkedHandles(functions.privateFunctions, linked)) {
        if (error) *error = nbError(@"foreign linked function");
        return false;
    }
    p->linked = linked.data();
    p->linked_count = visible;
    p->private_linked = linked.data() + visible;
    p->private_count = linked.size() - visible;
    p->function = [(NBFunction *)d.computeFunction handle];
    p->max_threads = d.maxTotalThreadsPerThreadgroup;
    p->multiple_of_execution_width = d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    p->indirect = d.supportIndirectCommandBuffers;
    p->max_call_stack_depth = d.maxCallStackDepth;
    p->adding_binary_functions = d.supportAddingBinaryFunctions;
    for (NSUInteger index = 0; index < 31; ++index) p->mutability[index] = d.buffers[index].mutability;
    return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property (nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc
{
    nmb_binary_archive_release(_handle);
}
- (NSUInteger)options
{
    return 0;
}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
    auto *p = new NmbRenderPipelineDescriptor();
    const bool ok = renderDescriptor(d, p, error) && nmb_binary_archive_add_render(_handle, p);
    delete p;
    if (!ok) return failWith<BOOL>(error, @"archive render pipeline", NO);
    if (error) *error = nil;
    return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error
{
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    if (!computeDescriptor(d, &p, linked, error)) return NO;
    if (!nmb_binary_archive_add_compute_descriptor(_handle, &p))
        return failWith<BOOL>(error, @"archive compute pipeline", NO);
    if (error) *error = nil;
    return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error
{
    if (!url.isFileURL) {
        if (error) *error = nbError(@"a file URL is required");
        return NO;
    }
    if (!nmb_binary_archive_serialize(_handle, url.fileSystemRepresentation))
        return failWith<BOOL>(error, @"archive serialize", NO);
    if (error) *error = nil;
    return YES;
}
@end

static bool pipelineArchives(NSArray *list, std::vector<NmbBinaryArchive *> &archives, NSError **error)
{
    for (id archive in list) {
        if (![archive isKindOfClass:[NBBinaryArchive class]]) {
            if (error) *error = nbError(@"foreign binary archive");
            return false;
        }
        archives.push_back([(NBBinaryArchive *)archive handle]);
    }
    return true;
}

// ---- the system's Metal front end ----
// Core Image makes its kernels at run time: 21H461's CoreImage imports newLibraryWithSource:options:error: (CIKL and generated kernels,
// CI::MetalMainProgram) and newLibraryWithDescriptorSPI:error: (its stitched DAGs, CI::StitchableFunctionDAGDescriptor; Core Image
// stitches by default, CI_ENABLE_METAL_DAG and CI_ENABLE_FUNCTION_STITCHING), and CoreUI draws every graphic icon (Settings' rows) through
// it. Metal sends both to MTLCompilerService, which has no target for this device. That service's front end, MTLCompiler.framework (in
// the shared cache: MTLCodeGenServiceCreate, MTLCodeGenServiceBuildRequest), answers the same requests in this process with a metallib
// of AIR, which is then loaded as any library. Request types (MTLCompilerObject::buildRequest's jump table): 13 Metal source, 14 a
// stitching DAG. As MaliMetal's frontend does on the same OS build. The reply, to a block called before the request returns, is a header
// whose u32s at 8 and 12 are its own size and that of the warnings after it, then the metallib. The framework is loaded on the first
// request: the render server, whose QuartzCore never compiles source, never loads it.
static std::string
    gCompileCache;  // <the runtime's cache>/msl: the compiler's replies, kept across respawns (empty: none kept)
static std::atomic<uint64_t> gCompiles, gCompilesKept, gCompileRefusals;
// The compiler was written for MTLCompilerService's threads and recurses deeply; a dispatch worker's 512 KiB stack was too small for it
// (MaliMetal: a stack-protector abort inside libGPUCompilerImplLazy). A request runs on a thread of its own with a 16 MiB stack, the
// caller waiting (the asynchronous forms below run on a queue already).
static void *runBlock(void *block)
{
    @autoreleasepool {
        ((__bridge void (^)(void))block)();
    }
    return nullptr;
}
static void onLargeStack(void (^body)(void))
{
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr)) {
        body();
        return;
    }
    const bool made = !pthread_attr_setstacksize(&attr, 16u << 20) &&
                      !pthread_create(&thread, &attr, runBlock, (__bridge void *)body);
    pthread_attr_destroy(&attr);
    if (made)
        pthread_join(thread, nullptr);
    else
        body();
}
// A reply depends only on the request and the compiler, so a compiled one is kept as <gCompileCache>/<SHA-256 of MTLCompiler's LC_UUID,
// the request type and its bytes>.reply: magic, warning and metallib lengths, the SHA-256 of both, then both. A respawned process (the
// icon service is ended whenever it goes idle) reads it instead of compiling again. Written beside its name and renamed; one that does
// not check out is compiled again; failures are not kept.
struct NBCompileReplyHeader {
    uint32_t magic, warningBytes;
    uint64_t metallibBytes;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
};
static const uint32_t kNBCompileReplyMagic = 0x3143534e;  // "NSC1"
static std::string compileReplyPath(const unsigned char uuid[16], uint32_t type, const std::vector<uint8_t> &request)
{
    if (gCompileCache.empty()) return std::string();
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, uuid, 16);
    CC_SHA256_Update(&context, &type, sizeof type);
    CC_SHA256_Update(&context, request.data(), (CC_LONG)request.size());
    unsigned char key[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(key, &context);
    char name[2 * CC_SHA256_DIGEST_LENGTH + 8];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(name + 2 * i, 3, "%02x", key[i]);
    return gCompileCache + "/" + name + ".reply";
}
static bool compileReplyRead(const std::string &path, NSData **metallib, NSString **warnings)
{
    NSData *file = path.empty()
                       ? nil
                       : [NSData dataWithContentsOfFile:@(path.c_str()) options:NSDataReadingMappedIfSafe error:nil];
    NBCompileReplyHeader header;
    if (file.length < sizeof header) return false;
    memcpy(&header, file.bytes, sizeof header);
    if (header.magic != kNBCompileReplyMagic || !header.metallibBytes ||
        sizeof header + header.warningBytes + header.metallibBytes != file.length)
        return false;
    const uint8_t *payload = (const uint8_t *)file.bytes + sizeof header;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(payload, (CC_LONG)(header.warningBytes + header.metallibBytes), digest);
    if (memcmp(digest, header.digest, sizeof digest)) return false;
    *warnings = header.warningBytes
                    ? [[NSString alloc] initWithBytes:payload length:header.warningBytes encoding:NSUTF8StringEncoding]
                    : nil;
    *metallib = [NSData dataWithBytes:payload + header.warningBytes length:(NSUInteger)header.metallibBytes];
    return true;
}
static void compileReplyWrite(const std::string &path, NSData *metallib, NSString *warnings)
{
    if (path.empty() || !metallib.length) return;
    NSData *warned = [warnings dataUsingEncoding:NSUTF8StringEncoding];
    NBCompileReplyHeader header = {kNBCompileReplyMagic, (uint32_t)warned.length, metallib.length, {}};
    NSMutableData *file = [NSMutableData dataWithCapacity:sizeof header + warned.length + metallib.length];
    [file appendBytes:&header length:sizeof header];
    if (warned.length) [file appendData:warned];
    [file appendData:metallib];
    uint8_t *bytes = (uint8_t *)file.mutableBytes;
    CC_SHA256(bytes + sizeof header, (CC_LONG)(warned.length + metallib.length),
              ((NBCompileReplyHeader *)bytes)->digest);
    mkdir(gCompileCache.c_str(), 0700);
    const std::string temporary = path + "." + std::to_string(getpid()) + ".tmp";
    if (![file writeToFile:@(temporary.c_str()) atomically:NO] || rename(temporary.c_str(), path.c_str()))
        unlink(temporary.c_str());
}
typedef void (^NBCodeGenReply)(int32_t status, const void *reply, size_t length, const char *message);
static NSData *codeGenRequest(uint32_t type, const std::vector<uint8_t> &request, NSString **warnings,
                              NSString **failure)
{
    static void *service;
    static void (*build)(void *, void *, uint32_t, const void *, size_t, NBCodeGenReply);
    static unsigned char compilerUuid[16];
    static dispatch_once_t once;
    static std::mutex lock;
    dispatch_once(&once, ^{
        void *library =
            dlopen("/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler", RTLD_NOW | RTLD_LOCAL);
        void *(*create)(const char *) =
            library ? (void *(*)(const char *))dlsym(library, "MTLCodeGenServiceCreate") : nullptr;
        build = library ? (void (*)(void *, void *, uint32_t, const void *, size_t, NBCodeGenReply))dlsym(
                              library, "MTLCodeGenServiceBuildRequest")
                        : nullptr;
        service = create && build ? create("native-metal") : nullptr;
        Dl_info info;
        if (build && dladdr((const void *)build, &info) && info.dli_fbase) {
            const mach_header_64 *header = (const mach_header_64 *)info.dli_fbase;
            const uint8_t *command = (const uint8_t *)(header + 1);
            for (uint32_t i = 0; i < header->ncmds; i++, command += ((const load_command *)command)->cmdsize)
                if (((const load_command *)command)->cmd == LC_UUID) {
                    memcpy(compilerUuid, ((const uuid_command *)command)->uuid, 16);
                    break;
                }
        }
        if (!service)
            NBLOG(@"native-metal: the system's Metal compiler (MTLCompiler.framework) did not load: %s",
                  library ? "no code generation service" : dlerror());
    });
    if (!service) {
        *failure = @"the system's Metal compiler (MTLCompiler.framework) is not available";
        return nil;
    }
    const std::string kept = compileReplyPath(compilerUuid, type, request);
    NSData *keptMetallib = nil;
    NSString *keptWarnings = nil;
    if (compileReplyRead(kept, &keptMetallib, &keptWarnings)) {
        gCompilesKept++;
        *warnings = keptWarnings;
        *failure = nil;
        return keptMetallib;
    }
    __block NSData *metallib = nil;
    __block NSString *warned = nil, *message = nil;
    __block bool called = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        onLargeStack(^{
            build(service, nullptr, type, request.data(), request.size(),
                  ^(int32_t status, const void *reply, size_t length, const char *why) {
                      called = true;
                      uint32_t header = 0, warning = 0;
                      if (reply && length >= 16) {
                          memcpy(&header, (const uint8_t *)reply + 8, 4);
                          memcpy(&warning, (const uint8_t *)reply + 12, 4);
                      }
                      if (status || !reply || length < 16 || (uint64_t)header + warning > length) {
                          message = why && *why
                                        ? @(why)
                                        : [NSString stringWithFormat:@"the Metal compiler failed (status %d)", status];
                          return;
                      }
                      if (warning)
                          warned = [[NSString alloc] initWithBytes:(const uint8_t *)reply + header
                                                            length:warning
                                                          encoding:NSUTF8StringEncoding];
                      metallib = [NSData dataWithBytes:(const uint8_t *)reply + header + warning
                                                length:length - header - warning];
                  });
        });
    }
    gCompiles++;
    if (!called) message = @"the Metal compiler did not answer";
    *warnings = warned;
    *failure = message;
    if (message) {
        gCompileRefusals++;
        return nil;
    }
    compileReplyWrite(kept, metallib, warned);
    return metallib;
}
// Metal source, request 13, as 21H461's front end reads it (buildRequest: u32 source bytes at 0, u32 bytes of an extra block at 4, flags
// at 12; the source from 16, padded to 4 bytes; the extra block; the options NUL-terminated): the source NUL-padded to a multiple of 4
// bytes, no extra block, the options' bytes at 8, no flags.
static NSData *compileMetalSource(NSString *source, NSString *arguments, NSString **warnings, NSString **failure)
{
    NSData *text = [source dataUsingEncoding:NSUTF8StringEncoding];
    const char *options = arguments.UTF8String;
    const uint32_t sourceBytes = (uint32_t)((text.length + 1 + 3) & ~(NSUInteger)3),
                   optionBytes = (uint32_t)strlen(options) + 1;
    std::vector<uint8_t> request(16 + sourceBytes + optionBytes, 0);
    const uint32_t head[4] = {sourceBytes, 0, optionBytes, 0};
    memcpy(request.data(), head, sizeof head);
    memcpy(request.data() + 16, text.bytes, text.length);
    memcpy(request.data() + 16 + sourceBytes, options, optionBytes);
    return codeGenRequest(13, request, warnings, failure);
}
// The front end's arguments for a set of compile options, as Metal passes them: the language version (3.1, this OS's, when none is
// given), fast math (Metal's default), invariance, size optimization and the preprocessor macros; module caches beside the replies.
static NSString *compileArgument(NSString *text)
{
    if ([text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\"\\'"]].location ==
        NSNotFound)
        return text;
    NSString *escaped = [[text stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
        stringByReplacingOccurrencesOfString:@"\""
                                  withString:@"\\\""];
    return [NSString stringWithFormat:@"\"%@\"", escaped];
}
static NSString *compileArguments(MTLCompileOptions *options)
{
    NSMutableArray<NSString *> *arguments =
        [NSMutableArray arrayWithObjects:@"--driver-mode=metal", @"-x", @"metal", nil];
    const NSUInteger version =
        options && options.languageVersion ? (NSUInteger)options.languageVersion : (NSUInteger)MTLLanguageVersion3_1;
    const unsigned major = (unsigned)(version >> 16), minor = (unsigned)(version & 0xffff);
    [arguments
        addObject:[NSString stringWithFormat:major >= 3 ? @"-std=metal%u.%u" : @"-std=ios-metal%u.%u", major, minor]];
    [arguments addObject:!options || options.fastMathEnabled ? @"-ffast-math" : @"-fno-fast-math"];
    if (options.preserveInvariance) [arguments addObject:@"-fpreserve-invariance"];
    if (options.optimizationLevel == MTLLibraryOptimizationLevelSize) [arguments addObject:@"-Os"];
    [options.preprocessorMacros enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSObject *value, BOOL *) {
        [arguments addObject:compileArgument([NSString stringWithFormat:@"-D%@=%@", name, value])];
    }];
    NSString *modules = gCompileCache.empty() ? [NSTemporaryDirectory() stringByAppendingPathComponent:@"m2v-msl"]
                                              : @((gCompileCache + "/modules").c_str());
    [arguments addObject:compileArgument([@"-fmodules-cache-path=" stringByAppendingString:modules])];
    return [arguments componentsJoinedByString:@" "];
}
// A refusal of a library request this file does not serve, logged for the first few (which of Core Image's paths a process takes).
static void refuseLibrary(NSString *what)
{
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 8) NBLOG(@"native-metal: %@ refused (not implemented)", what);
}
// Whether this process compiles Metal source and stitched libraries (NMB_SYSTEM_COMPILER=1, which the plugin sets in every process but the
// render server: its requests stay refused as before, so it never loads the compiler or waits for one).
static bool systemCompiler()
{
    static const bool on = [] {
        const char *v = getenv("NMB_SYSTEM_COMPILER");
        return v && strcmp(v, "0");
    }();
    return on;
}
// The target's own _MTLDevice method that writes stitching graphs as the DAG text Metal's compiler reads (the device's base class).
@protocol NBOriginalStitching
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end
// The base class's limits table (-[_MTLDevice limits], the words after isa in 21H461). Metal's accessors read their field from it
// (-[_MTLDevice maxTextureWidth2D] is limits+0x84), and frameworks read it directly, past this device's methods: PhotosUICore's texture
// converter takes its largest image from 0x84/0x88 (-[PXGMetalTextureConverter createTextureFromCGImage:transform:alpha:options:],
// PXGCreateMetalTextureFromBytesInCGImage); with them 0 every image was "too large", was redrawn to fit 0x0 and PXGCreateContext asserted
// (r5d: Photos aborted twice on launch). CoreRE reads minBufferNoCopyAlignmentBytes at 0x7c. The offsets are those accessors' loads (the
// 61 integer fields; maxLineWidth and maxPointSize are floats, not filled).
@protocol NBOriginalLimits
- (void *)limits;
@end
#if TARGET_OS_IPHONE
static const struct {
    const char *selector;
    uint16_t offset;
} kLimitFields[] = {{"maxFramebufferStorageBits", 0x0},
                    {"linearTextureArrayAlignmentBytes", 0x4},
                    {"linearTextureArrayAlignmentSlice", 0x8},
                    {"maxTileBuffers", 0xc},
                    {"maxTileTextures", 0x10},
                    {"maxTileSamplers", 0x14},
                    {"maxTileInlineDataSize", 0x18},
                    {"minTilePixels", 0x1c},
                    {"maxColorAttachments", 0x20},
                    {"maxVertexAttributes", 0x24},
                    {"maxVertexBuffers", 0x28},
                    {"maxVertexTextures", 0x2c},
                    {"maxVertexSamplers", 0x30},
                    {"maxVertexInlineDataSize", 0x34},
                    {"maxInterpolants", 0x38},
                    {"maxFragmentBuffers", 0x3c},
                    {"maxFragmentTextures", 0x40},
                    {"maxFragmentSamplers", 0x44},
                    {"maxFragmentInlineDataSize", 0x48},
                    {"maxComputeBuffers", 0x4c},
                    {"maxComputeTextures", 0x50},
                    {"maxComputeSamplers", 0x54},
                    {"maxComputeInlineDataSize", 0x58},
                    {"maxComputeLocalMemorySizes", 0x5c},
                    {"maxTotalComputeThreadsPerThreadgroup", 0x60},
                    {"maxComputeThreadgroupMemory", 0x64},
                    {"maxVisibilityQueryOffset", 0x70},
                    {"minConstantBufferAlignmentBytes", 0x78},
                    {"minBufferNoCopyAlignmentBytes", 0x7c},
                    {"maxTextureWidth1D", 0x80},
                    {"maxTextureWidth2D", 0x84},
                    {"maxTextureHeight2D", 0x88},
                    {"maxTextureWidth3D", 0x8c},
                    {"maxTextureHeight3D", 0x90},
                    {"maxTextureDepth3D", 0x94},
                    {"maxTextureDimensionCube", 0x98},
                    {"maxTextureLayers", 0x9c},
                    {"linearTextureAlignmentBytes", 0xa0},
                    {"iosurfaceTextureAlignmentBytes", 0xa4},
                    {"iosurfaceReadOnlyTextureAlignmentBytes", 0xa8},
                    {"deviceLinearTextureAlignmentBytes", 0xac},
                    {"deviceLinearReadOnlyTextureAlignmentBytes", 0xb0},
                    {"maxFunctionConstantIndices", 0xb4},
                    {"maxComputeThreadgroupMemoryAlignmentBytes", 0xb8},
                    {"maxInterpolatedComponents", 0xbc},
                    {"maxTessellationFactor", 0xc0},
                    {"maxIndirectBuffers", 0xc4},
                    {"maxIndirectTextures", 0xc8},
                    {"maxIndirectSamplers", 0xcc},
                    {"maxIndirectSamplersPerDevice", 0xd0},
                    {"maxFenceInstances", 0xd4},
                    {"maxViewportCount", 0xd8},
                    {"maxCustomSamplePositions", 0xdc},
                    {"maxVertexAmplificationFactor", 0xe0},
                    {"maxVertexAmplificationCount", 0xe4},
                    {"maxTextureBufferWidth", 0xe8},
                    {"maxComputeAttributes", 0xec},
                    {"maxIOCommandsInFlight", 0xf0},
                    {"maxPredicatedNestingDepth", 0xf4},
                    {"maxAccelerationStructureLevels", 0xf8},
                    {"maxConstantBufferArguments", 0xfc}};
// Each field whose selector this device answers itself (installAnswers, NBDeviceMethods) gets that answer, so the table says what the
// methods say; one the device has no answer for stays the base class's 0. Every write is read back through the base class's own accessor,
// and a field it does not read back is restored and the fill stops there (a different layout), the field named.
static void fillLimits(id device)
{
    Class cls = object_getClass(device), base = class_getSuperclass(cls);
    if (![base instancesRespondToSelector:@selector(limits)]) return;
    uint8_t *table = (uint8_t *)[(id<NBOriginalLimits>)device limits];
    if (!table) return;
    for (const auto &field : kLimitFields) {
        SEL sel = sel_registerName(field.selector);
        Method own = class_getInstanceMethod(cls, sel), original = class_getInstanceMethod(base, sel);
        if (!own || !original || method_getImplementation(own) == method_getImplementation(original)) continue;
        char type[8] = {};
        method_getReturnType(own, type, sizeof type);
        if (strcmp(type, "Q") && strcmp(type, "L") && strcmp(type, "I")) continue;
        const uint64_t value = ((NSUInteger (*)(id, SEL))method_getImplementation(own))(device, sel);
        if (value > UINT32_MAX) {
            NBLOG(@"native-metal: limits: %s %llu does not fit the table", field.selector, (unsigned long long)value);
            continue;
        }
        uint32_t was;
        memcpy(&was, table + field.offset, 4);
        const uint32_t word = (uint32_t)value;
        memcpy(table + field.offset, &word, 4);
        if (((NSUInteger (*)(id, SEL))method_getImplementation(original))(device, sel) != value) {
            memcpy(table + field.offset, &was, 4);
            NBLOG(@"native-metal: limits: the base class does not read %s at 0x%x; the fields from there stay 0",
                  field.selector, field.offset);
            return;
        }
    }
    uint32_t width = 0, height = 0;
    memcpy(&width, table + 0x84, 4);
    memcpy(&height, table + 0x88, 4);
    if (!width || !height)
        NBLOG(@"native-metal: limits: no 2D texture size from the device (PhotosUICore refuses every image)");
}
#else
static void fillLimits(id) {}  // the host's _MTLDevice has its own layout
#endif

@interface NBDeviceMethods : NSObject
@end
@implementation NBDeviceMethods
- (NSString *)name
{
    return gName;
}
- (uint64_t)registryID
{
    return gRegistryID;
}
// The kernel driver's largest buffer object (max_bo_bytes, 256 MiB; CONTRACT-kernel-notes.md) bounds a buffer below NVK's
// maxMemoryAllocationSize (2 GiB): MPS and MPSGraph size their tiles and scratch from this answer.
- (NSUInteger)maxBufferLength
{
    return std::min<uint64_t>(nmb_device_max_buffer_length(gDevice), 256ull << 20);
}
- (BOOL)hasUnifiedMemory
{
    return YES;
}
- (BOOL)isLowPower
{
    return NO;
}
- (BOOL)isRemovable
{
    return NO;
}
- (BOOL)isHeadless
{
    return NO;
}
// Unified memory: all of it is shared, none dedicated (Core Image's CIMetalDeviceGetLimits sizes its caches from these).
- (uint64_t)sharedMemorySize
{
    return NSProcessInfo.processInfo.physicalMemory;
}
- (uint64_t)dedicatedMemorySize
{
    return 0;
}
// A no-copy buffer wraps whole pages (nmb_buffer_new_no_copy), the kernel's 4 KiB.
- (NSUInteger)minBufferNoCopyAlignmentBytes
{
    return (NSUInteger)getpagesize();
}
// MPS sizes its threadgroup memory from this private total (its MPSDevice reads it unguarded); the runtime has no answer of its own
// (Metal's is Apple's), so it is the public length the runtime enforces, never more than a dispatch can take.
- (NSUInteger)maxComputeThreadgroupMemory
{
    return [(id<MTLDevice>)self maxThreadgroupMemoryLength];
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    uint64_t a[2] = {};
    if (!nmb_device_heap_buffer_size(gDevice, length, options, a))
        NBLOG(@"native-metal: heapBufferSizeAndAlign: %@", lastError());
    return {a[0], a[1]};
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
    NmbTextureDescriptor descriptor;
    uint64_t a[2] = {};
    if (!textureDescriptor(d, &descriptor) || !nmb_device_heap_texture_size(gDevice, &descriptor, a))
        NBLOG(@"native-metal: heapTextureSizeAndAlign: %@", lastError());
    return {a[0], a[1]};
}
// Metal source and stitched libraries are compiled by the system's front end in this process (codeGenRequest, above); the base class
// would send them to Apple's compiler service, which has no target for this device. Core Image's filter-function linking and the DAG form
// stay refused.
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    if (!systemCompiler()) {
        if (error) *error = nbError(@"Metal source is not compiled here (AIR libraries only)");
        return nil;
    }
    if (!source) {
        if (error) *error = nbError(@"no source");
        return nil;
    }
    if (options && options.libraryType == MTLLibraryTypeDynamic) {
        if (error) *error = nbError(@"dynamic libraries are not supported");
        return nil;
    }
    NSString *warnings = nil, *failure = nil;
    NSData *metallib = compileMetalSource(source, compileArguments(options), &warnings, &failure);
    if (!metallib) {
        NBLOG(@"native-metal: newLibraryWithSource: %@",
              [failure substringToIndex:MIN(failure.length, (NSUInteger)300)]);
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey : failure}];
        return nil;
    }
    dispatch_data_t data = dispatch_data_create(metallib.bytes, metallib.length, nullptr, ^{ (void)metallib; });
    id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithData:data error:error];
    // As Metal does: a library compiled with warnings comes with them as a CompileWarning error.
    if (library && warnings.length && error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorCompileWarning
                                 userInfo:@{NSLocalizedDescriptionKey : warnings}];
    return library;
}
- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithSource:source options:options error:&error];
        if (handler) handler(library, error);
    });
}
// A stitched library, request 14, as 21H461's front end reads it (MTLCompilerObject::stitchFunctionDag): 'dag ' and the graphs' DAG
// text NUL-terminated, 'numf' and the u32 count of functions, then for each function 'bitc', its u32 size and its wrapped bitcode. A
// newer front end (macOS 27's, the host instrument's) also reads 'nums', 'ptfm' and 'arid' words after them and aborts without them:
// they are sent as zeros, which 21H461's never reads. Core Image's SPI descriptor (newLibraryWithDescriptorSPI:, MTLStitchedLibraryDescriptorSPI, its graphs with SPI
// texture and builtin nodes) is the same class family; the base class's newDagStringWithGraphs: writes its DAG text.
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    if (!systemCompiler()) {
        if (error) *error = nbError(@"function stitching is not implemented");
        return nil;
    }
    if (![d respondsToSelector:@selector(functionGraphs)] || ![d respondsToSelector:@selector(functions)]) {
        if (error) *error = nbError(@"not a stitched library descriptor");
        return nil;
    }
    NSArray<MTLFunctionStitchingGraph *> *graphs = d.functionGraphs;
    NSArray<id<MTLFunction>> *functions = d.functions;
    if (!graphs.count || !functions.count) {
        if (error) *error = nbError(@"a stitched library needs graphs and functions");
        return nil;
    }
    if (![self respondsToSelector:@selector(newDagStringWithGraphs:)]) {
        if (error) *error = nbError(@"no DAG writer in the base device");
        return nil;
    }
    NSString *dag = [(id<NBOriginalStitching>)self newDagStringWithGraphs:graphs];
    if (!dag.length) {
        if (error) *error = nbError(@"the stitching graphs have no DAG text");
        return nil;
    }
    std::vector<uint8_t> request;
    auto word = [&](uint32_t v) {
        const uint8_t *b = (const uint8_t *)&v;
        request.insert(request.end(), b, b + 4);
    };
    const char *text = dag.UTF8String;
    word(0x64616720 /* 'dag ' */);
    request.insert(request.end(), text, text + strlen(text) + 1);
    word(0x6e756d66 /* 'numf' */);
    word((uint32_t)functions.count);
    for (id function in functions) {
        NSData *bitcode = [function isKindOfClass:[NBFunction class]] ? functionBitcode((NBFunction *)function) : nil;
        if (!bitcode) {
            if (error)
                *error =
                    nbError([NSString stringWithFormat:@"stitched function %@ has no bitcode here", [function name]]);
            return nil;
        }
        word(0x62697463 /* 'bitc' */);
        word((uint32_t)bitcode.length);
        const uint8_t *b = (const uint8_t *)bitcode.bytes;
        request.insert(request.end(), b, b + bitcode.length);
    }
    word(0x6e756d73 /* 'nums' */);
    word(0);
    word(0x7074666d /* 'ptfm' */);
    word(0);
    word(0x61726964 /* 'arid' */);
    word(0);
    NSString *warnings = nil, *failure = nil;
    NSData *metallib = codeGenRequest(14, request, &warnings, &failure);
    if (!metallib) {
        NBLOG(@"native-metal: stitched library: %@", [failure substringToIndex:MIN(failure.length, (NSUInteger)300)]);
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey : failure}];
        return nil;
    }
    dispatch_data_t data = dispatch_data_create(metallib.bytes, metallib.length, nullptr, ^{ (void)metallib; });
    return [(id<MTLDevice>)self newLibraryWithData:data error:error];
}
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                          destinationBinaryArchive:(id)archive
                                             error:(NSError **)error
{
    (void)archive;
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}
- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                       completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:&error];
        if (handler) handler(library, error);
    });
}
// Core Image's stitched DAGs (CreateStitchedFunctionFromDescriptor); the base class forwards these two to the destinationBinaryArchive:
// form, answered above. Made here so the route does not depend on it.
- (id<MTLLibrary>)newLibraryWithDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}
- (id<MTLLibrary>)newLibraryWithStitchedDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}
- (id<MTLLibrary>)newLibraryWithDAG:(id)dag functions:(NSArray *)functions error:(NSError **)error
{
    (void)dag;
    (void)functions;
    refuseLibrary(@"function stitching (DAG)");
    if (error) *error = nbError(@"function stitching (DAG) is not implemented");
    return nil;
}
// Core Image's legacy DAG (CI::LegacyDAGDescriptor, CreateFunctionFromDAG) and kernel reflection (CIKernelReflection::reflectConstants).
- (id<MTLLibrary>)newLibraryWithImageFilterFunctionsSPI:(id)functions
                                imageFilterFunctionInfo:(id)info
                                                  error:(NSError **)error
{
    (void)functions;
    (void)info;
    refuseLibrary(@"Core Image filter-function linking");
    if (error) *error = nbError(@"Core Image filter-function linking is not implemented");
    return nil;
}
- (id<MTLLibrary>)newLibraryWithCIFilters:(id)functions imageFilterFunctionInfo:(id)info error:(NSError **)error
{
    (void)functions;
    (void)info;
    refuseLibrary(@"Core Image filter-function linking");
    if (error) *error = nbError(@"Core Image filter-function linking is not implemented");
    return nil;
}
- (BOOL)supportsDynamicLibraries
{
    return NO;
}
// A pipeline library (RenderBox's default.pipelinelib; TextInputUI, PencilKit, SiriUICore and others have their own) is pipelines.desc, the
// functions and pipelines.<target>: one Apple GPU's compiled pipelines. The base class's MTLPipelineLibraryBuilder::newLibraryWithFile names
// that file from -targetDeviceInfo, which _MTLDevice answers NULL (only a device that does not respond to it gets a stand-in), and read
// NULL+0x10 at Metal+0x52b64 (r5d: every WidgetRenderer_Default and MobileSafari, from RB::Device::render_pipeline_state). A library holds
// no pipelines for this GPU, so it is refused as Metal refuses one without the device's target file; RenderBox and the others then build
// their pipelines from their own metallib.
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
    refuseLibrary([NSString stringWithFormat:@"pipeline library %@", path.lastPathComponent]);
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorFileNotFound
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         @"a pipeline library holds Apple GPUs' pipelines, none for this device"
                                 }];
    return nil;
}
// A buffer over a surface's pages (Espresso, MPSCore and MPSGraph pass IOSurface-backed tensors this way), the import textures use.
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface
{
    if (!surface || IOSurfaceGetPlaneCount(surface)) return nil;
    void *base = IOSurfaceGetBaseAddress(surface);
    const size_t page = (size_t)getpagesize(), allocation = IOSurfaceGetAllocSize(surface);
    if (!base || ((uintptr_t)base & (page - 1)) || !allocation || allocation > SIZE_MAX - (page - 1)) return nil;
    return (id<MTLBuffer>)surfaceBuffer(surface, base, (allocation + page - 1) & ~(page - 1));
}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
// The legacy feature sets of the same families (iOS GPU families 1 to 3, every version this OS defines), so a framework that still asks
// the old question (MPS's and Core Image's device checks) gets the answer supportsFamily: gives; the base implementation would read
// Apple tables this device does not have. Family 4 and up (imageblocks, tile shaders, raster order groups) are not claimed.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set
{
    switch ((NSUInteger)set) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 12:
    case 13:
    case 14: return YES;
    default: return NO;
    }
}
// The layout Metal gives these descriptors (the runtime's ArgumentBufferLayout); a data type or alignment Metal raises at is refused.
- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
    std::vector<NmbArgumentDescriptor> records;
    for (MTLArgumentDescriptor *a in arguments)
        records.push_back({(uint64_t)a.dataType, a.index, a.arrayLength, (uint64_t)a.access, (uint64_t)a.textureType,
                           a.constantBlockAlignment});
    return [NBArgumentEncoder
        with:records.empty() ? nullptr : nmb_device_new_argument_encoder(gDevice, records.data(), records.size())];
}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. Here the limits table gets this device's own answers instead (fillLimits,
// also run when the device opens) and the other three are empty, so a feature stays the base class's neutral NO and a limit the device has no
// answer for its 0. Nothing is claimed that nothing measured.
- (void)initLimits
{
    fillLimits(self);
}
- (void)initFeatureQueries
{}
- (void)initGPUFamilySupport
{}
- (void)initWorkarounds
{}
- (NSString *)productName
{
    return gName;
}
- (NSString *)vendorName
{
    char name[64] = {};
    nmb_device_vendor_name(gDevice, name, sizeof name);
    return [NSString stringWithUTF8String:name];
}
- (MTLSize)maxThreadsPerThreadgroup
{
    uint64_t s[3] = {};
    if (!nmb_device_max_threads_per_threadgroup(gDevice, s))
        NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@", lastError());
    return MTLSizeMake(s[0], s[1], s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    if (gMissing) gMissing("NativeBridgeMetalDevice", sel_getName(selector));
    noteProblem([NSString stringWithFormat:@"missing device %s", sel_getName(selector)]);
    [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent
{
    NmbEvent *handle = nmb_event_new(gDevice, false);
    if (!handle) return nil;
    NBEvent *event = [NBEvent new];
    event.handle = handle;
    return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent
{
    NmbEvent *handle = nmb_event_new(gDevice, true);
    if (!handle) return nil;
    NBSharedEvent *event = [NBSharedEvent new];
    event.handle = handle;
    return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence
{
    NmbFence *handle = nmb_fence_new(gDevice);
    if (!handle) return nil;
    NBFence *fence = [NBFence new];
    fence.handle = handle;
    return (id<MTLFence>)fence;
}
- (id<MTLCommandQueue>)newCommandQueue
{
    NmbQueue *handle = nmb_queue_new(gDevice);
    if (!handle) return nil;
    NBQueue *queue = [NBQueue new];
    queue.handle = handle;
    return (id<MTLCommandQueue>)queue;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    if (!d) return nil;
    NmbHeap *handle = nmb_heap_new(gDevice, d.type, d.size, (uint64_t)d.resourceOptions);
    if (!handle) {
        NBLOG(@"native-metal: heap refused: %@", lastError());
        return nil;
    }
    NBHeap *heap = [NBHeap new];
    heap.handle = handle;
    heap.options = d.resourceOptions;
    heap.heapType = d.type;
    return (id<MTLHeap>)heap;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained[])resources count:(NSUInteger)count
{
    NBResourceGroup *group = [NBResourceGroup new];
    NSMutableArray *held = [NSMutableArray arrayWithCapacity:count];
    for (NSUInteger i = 0; i < count; i++)
        if (resources[i]) [held addObject:resources[i]];
    group.resources = held;
    return group;
}
// Not made here: a texture over raw memory in Apple's layout, and Apple's texture layout object. Both are refused, with the reason, rather than
// answered with a guessed shape.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)d
                                deallocator:(void (^)(void *, NSUInteger))deallocator
{
    (void)bytes;
    (void)length;
    (void)d;
    (void)deallocator;
    NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture "
          @"layout of raw memory is not known)");
    return nil;
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed
{
    (void)d;
    (void)backed;
    NBLOG(@"native-metal: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: is not implemented (Apple's layout "
          @"object is not known)");
    return nil;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count
{
    (void)count;
    return [(id<MTLDevice>)self newCommandQueue];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    NmbBuffer *handle = nmb_buffer_new(gDevice, length, options, nullptr);
    if (!handle) {
        NBLOG(@"native-metal: buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
    if (!bytes) return nil;
    NmbBuffer *handle = nmb_buffer_new(gDevice, length, options, bytes);
    if (!handle) {
        NBLOG(@"native-metal: buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    NmbBuffer *handle = nmb_buffer_new_no_copy(gDevice, pointer, length, options);
    if (!handle) {
        NBLOG(@"native-metal: no-copy buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    buffer.wrapped = pointer;
    buffer.deallocator = deallocator;
    return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor)) return nil;
    NmbTexture *handle = nmb_texture_new(gDevice, &descriptor);
    noteFormat((uint32_t)descriptor.format, handle != nullptr);
    if (!handle) {
        NBLOG(@"native-metal: texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    return (id<MTLTexture>)texture;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                 iosurface:(IOSurfaceRef)surface
                                     plane:(NSUInteger)plane
{
    if (surface && d && (plane || IOSurfaceGetPlaneCount(surface))) {
        // A planar surface (a decoded HEIC/HEVC frame, '420v'/'420f', Metal format 500 and its kin) is not imported here: say each kind once,
        // so a boot shows whether the render server is handed any (the wallpaper's two-plane YCbCr image was evb10's largest GPU cost).
        gSurfaceMultiPlane++;
        static NSMutableSet *said = [NSMutableSet set];
        const uint32_t fourcc = IOSurfaceGetPixelFormat(surface);
        NSString *kind = [NSString stringWithFormat:@"%lu %c%c%c%c %zu", (unsigned long)d.pixelFormat,
                                                    (char)(fourcc >> 24), (char)(fourcc >> 16), (char)(fourcc >> 8),
                                                    (char)fourcc, IOSurfaceGetPlaneCount(surface)];
        bool first = false;
        @synchronized(said) {
            if (![said containsObject:kind]) {
                [said addObject:kind];
                first = true;
            }
        }
        if (first) NBLOG(@"native-metal: planar surface texture refused (format, fourcc, planes): %@", kind);
        return nil;
    }
    if (!surface || !d) return nil;
    void *base = IOSurfaceGetBaseAddress(surface);
    const size_t page = (size_t)getpagesize(), allocation = IOSurfaceGetAllocSize(surface);
    if (!page || (page & (page - 1)) || !base || ((uintptr_t)base & (page - 1)) || !allocation ||
        allocation > SIZE_MAX - (page - 1))
        return nil;
    const size_t bytes = (allocation + page - 1) & ~(page - 1);
    NBBuffer *buffer = surfaceBuffer(surface, base, bytes);
    if (!buffer) return nil;
    NBTexture *texture = textureOnBuffer(buffer, d, 0, IOSurfaceGetBytesPerRow(surface), true);
    if (!texture) return nil;
    texture.surface = (IOSurfaceRef)CFRetain(surface);
    return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    const uint64_t libraryBegan = nowUs();
    NmbLibrary *handle = nmb_library_new(gDevice, bytes, size, false);
    gTimeLibrary.add(libraryBegan);
    if (!handle) return failWith<id<MTLLibrary>>(error, @"library", nil);
    NBLibrary *library = [NBLibrary new];
    library.handle = handle;
    library.storage = mapped;
    library.bytes = (const uint8_t *)bytes;
    library.length = size;
    if (error) *error = nil;
    return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    NSData *bytes = path ? [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error] : nil;
    if (!bytes) return nil;
    const uint64_t libraryBegan = nowUs();
    NmbLibrary *handle = nmb_library_new(gDevice, bytes.bytes, bytes.length, false);
    gTimeLibrary.add(libraryBegan);
    if (!handle) return failWith<id<MTLLibrary>>(error, @"library", nil);
    NBLibrary *library = [NBLibrary new];
    library.handle = handle;
    library.storage = bytes;
    library.bytes = (const uint8_t *)bytes.bytes;
    library.length = bytes.length;
    if (error) *error = nil;
    return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if (!url.isFileURL) {
        if (error) *error = nbError(@"a file URL is required");
        return nil;
    }
    return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url) {
        if (error) *error = nbError(@"the bundle has no default.metallib");
        return nil;
    }
    return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary
{
    return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];
}
// The asynchronous forms make the state on the caller's thread and call back (Core Image's, Espresso's): the base class would queue a
// request to Apple's compiler service.
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                          completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
    NSError *error = nil;
    id<MTLComputePipelineState> state = [(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];
    handler(state, error);
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                      options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
    NSError *error = nil;
    id<MTLComputePipelineState> state = [(id<MTLDevice>)self newComputePipelineStateWithDescriptor:d
                                                                                           options:options
                                                                                        reflection:nil
                                                                                             error:&error];
    handler(state, nil, error);
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
    return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                           options:(MTLPipelineOption)options
                                                        reflection:
                                                            (MTLAutoreleasedComputePipelineReflection *)reflection
                                                             error:(NSError **)error
{
    if (reflection) *reflection = nil;
    if (![(id)function isKindOfClass:[NBFunction class]]) {
        if (error) *error = nbError(@"foreign function");
        return nil;
    }
    const NmbPipelineOptions creation = {(uint64_t)options, nullptr, 0};
    const uint64_t pipelineBegan = nowUs();
    NmbComputePipeline *handle =
        nmb_compute_pipeline_new_with_options(gDevice, [(NBFunction *)function handle], &creation);
    gTimePipeline.add(pipelineBegan);
    if (!handle) return failWith<id<MTLComputePipelineState>>(error, @"compute pipeline", nil);
    NBComputePipeline *state = [NBComputePipeline new];
    state.handle = handle;
    gPipelines++;
    if (error) *error = nil;
    return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                             options:(MTLPipelineOption)options
                                                          reflection:
                                                              (MTLAutoreleasedComputePipelineReflection *)reflection
                                                               error:(NSError **)error
{
    if (reflection) *reflection = nil;
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    if (!computeDescriptor(d, &p, linked, error)) return nil;
    std::vector<NmbBinaryArchive *> archives;
    if (!pipelineArchives(d.binaryArchives, archives, error)) return nil;
    const NmbPipelineOptions creation = {(uint64_t)options, archives.data(), archives.size()};
    const uint64_t pipelineBegan = nowUs();
    NmbComputePipeline *handle = nmb_compute_pipeline_new_with_descriptor(gDevice, &p, &creation);
    gTimePipeline.add(pipelineBegan);
    if (!handle) return failWith<id<MTLComputePipelineState>>(error, @"compute pipeline", nil);
    NBComputePipeline *state = [NBComputePipeline new];
    state.handle = handle;
    state.label = d.label;
    gPipelines++;
    if (error) *error = nil;
    return (id<MTLComputePipelineState>)state;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d
                                                             options:MTLPipelineOptionNone
                                                          reflection:nil
                                                               error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    if (reflection) *reflection = nil;
    auto *p = new NmbRenderPipelineDescriptor();
    if (!renderDescriptor(d, p, error)) {
        delete p;
        return nil;
    }
    std::vector<NmbBinaryArchive *> archives;
    if (!pipelineArchives(d.binaryArchives, archives, error)) {
        delete p;
        return nil;
    }
    const NmbPipelineOptions creation = {(uint64_t)options, archives.data(), archives.size()};
    const uint64_t pipelineBegan = nowUs();
    const NBCost costBegan = NBCost::now();
    NmbRenderPipeline *handle = nmb_render_pipeline_new_with_options(gDevice, p, &creation);
    delete p;
    gTimePipeline.add(pipelineBegan);
    gHistPso.add(nowUs() - pipelineBegan);
    journalRenderPipeline(d, pipelineBegan, handle ? nullptr : nmb_last_error(),
                          gJournalPath.empty() ? std::string() : NBCost::now().since(costBegan));
    if (!handle) return failWith<id<MTLRenderPipelineState>>(error, @"render pipeline", nil);
    NBRenderPipeline *state = [NBRenderPipeline new];
    state.handle = handle;
    gPipelines++;
    if (error) *error = nil;
    return (id<MTLRenderPipelineState>)state;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    if (d.url && !d.url.isFileURL) {
        if (error) *error = nbError(@"a file URL is required");
        return nil;
    }
    NmbBinaryArchive *handle = nmb_binary_archive_new(gDevice, d.url ? d.url.fileSystemRepresentation : nullptr);
    if (!handle) return failWith<id<MTLBinaryArchive>>(error, @"binary archive", nil);
    NBBinaryArchive *archive = [NBBinaryArchive new];
    archive.handle = handle;
    if (error) *error = nil;
    return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d
{
    const uint64_t row = nmb_device_linear_bytes_per_row(gDevice, d.pixelFormat, d.width);
    if (!row) NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@", lastError());
    return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    NmbSamplerDescriptor s = {(uint64_t)d.minFilter,
                              (uint64_t)d.magFilter,
                              (uint64_t)d.mipFilter,
                              (uint64_t)d.sAddressMode,
                              (uint64_t)d.tAddressMode,
                              (uint64_t)d.rAddressMode,
                              (uint64_t)d.borderColor,
                              (uint64_t)d.normalizedCoordinates,
                              d.maxAnisotropy,
                              (uint64_t)d.compareFunction,
                              (uint64_t)d.lodAverage,
                              d.lodMinClamp,
                              d.lodMaxClamp};
    NmbSampler *handle = nmb_sampler_new(gDevice, &s);
    if (!handle) {
        NBLOG(@"native-metal: sampler refused: %@", lastError());
        return nil;
    }
    NBSampler *sampler = [NBSampler new];
    sampler.handle = handle;
    return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    auto face = [](MTLStencilDescriptor *s) -> NmbStencil {
        return {(uint64_t)s.stencilCompareFunction,
                (uint64_t)s.stencilFailureOperation,
                (uint64_t)s.depthFailureOperation,
                (uint64_t)s.depthStencilPassOperation,
                s.readMask,
                s.writeMask};
    };
    NmbDepthStencilDescriptor s = {(uint64_t)d.depthCompareFunction, (uint64_t)d.depthWriteEnabled,
                                   face(d.frontFaceStencil), face(d.backFaceStencil)};
    NmbDepthStencil *handle = nmb_depth_stencil_new(gDevice, &s);
    if (!handle) {
        NBLOG(@"native-metal: depth/stencil state refused: %@", lastError());
        return nil;
    }
    NBDepthStencil *state = [NBDepthStencil new];
    state.handle = handle;
    return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime once per argument and keeps the answer
// (each is a fixed property of the device, and QuartzCore asks thousands a second while the screen moves: about 140 us each on Adreno,
// AdrenoMetal f875e531), a refusal answering 0/NO with the reason logged once. NMB_ANSWER_CACHE=0 asks the runtime every call again.
static std::unordered_map<SEL, uint64_t> gAnswers;
static NSUInteger countAnswer(id, SEL cmd)
{
    return (NSUInteger)gAnswers.at(cmd);
}
static BOOL flagAnswer(id, SEL cmd)
{
    return gAnswers.at(cmd) != 0;
}
static uint64_t askAnswer(SEL cmd, NSUInteger arg)
{
    static const bool keep = [] {
        const char *v = getenv("NMB_ANSWER_CACHE");
        return !v || strcmp(v, "0");
    }();
    static std::mutex lock;
    static std::unordered_map<SEL, std::unordered_map<NSUInteger, uint64_t>> kept;
    if (keep) {
        std::lock_guard<std::mutex> hold(lock);
        auto answers = kept.find(cmd);
        if (answers != kept.end()) {
            auto answer = answers->second.find(arg);
            if (answer != answers->second.end()) {
                gAnswersKept++;
                return answer->second;
            }
        }
    }
    gAnswersAsked++;
    uint64_t value = 0;
    if (!nmb_device_answer(gDevice, sel_getName(cmd), arg, &value)) {
        NBLOG(@"native-metal: %s(%lu): %@", sel_getName(cmd), (unsigned long)arg, lastError());
        value = 0;
    }
    if (keep) {
        std::lock_guard<std::mutex> hold(lock);
        kept[cmd][arg] = value;
    }
    return value;
}
static NSUInteger countArgAnswer(id, SEL cmd, NSUInteger arg)
{
    return (NSUInteger)askAnswer(cmd, arg);
}
static BOOL flagArgAnswer(id, SEL cmd, NSUInteger arg)
{
    return askAnswer(cmd, arg) != 0;
}
static void installAnswers(Class cls)
{
    uint32_t kind = 0;
    for (size_t i = 0; const char *name = nmb_device_answer_selector(i, &kind); i++) {
        SEL sel = sel_registerName(name);
        uint64_t value = 0;
        switch (kind) {
        case NMB_ANSWER_COUNT:
        case NMB_ANSWER_FLAG:
            if (!nmb_device_answer(gDevice, name, 0, &value)) {
                NBLOG(@"native-metal: no device answer: %@", lastError());
                continue;
            }
            // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
            // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
            // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
            if (!strcmp(name, "supportsExtendedXR10Formats")) value = 0;
            // Command buffer jumps are withheld: on YES MPSGraph and MPSImageCanny send the AGX encoder's encodeStartWhile/If/Else/EndIf SPI,
            // which this frontend's encoders do not implement.
            if (!strcmp(name, "supportsCommandBufferJump")) value = 0;
            gAnswers[sel] = value;
            class_addMethod(cls, sel, kind == NMB_ANSWER_COUNT ? (IMP)countAnswer : (IMP)flagAnswer,
                            kind == NMB_ANSWER_COUNT ? "Q16@0:8" : "B16@0:8");
            break;
        case NMB_ANSWER_COUNT_ARG: class_addMethod(cls, sel, (IMP)countArgAnswer, "Q24@0:8Q16"); break;
        case NMB_ANSWER_FLAG_ARG: class_addMethod(cls, sel, (IMP)flagArgAnswer, "B24@0:8Q16"); break;
        }
    }
}

extern "C" id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID,
                                       void (*missing)(const char *, const char *))
{
    if (gDeviceObject) return gDeviceObject;
    gMissing = missing;
    gInFlight = [NSMutableSet set];
    gDevice = nmb_device_open(loader, cache);
    if (!gDevice) {
        NBLOG(@"native-metal: device refused: %s", nmb_last_error());
        return nil;
    }
    if (cache) {
        gCompileCache = std::string(cache) + "/msl";
        // The journal now lives in a cache that survives reboots: past 2 MiB it starts again, the previous one kept as pipelines.log.old.
        gJournalPath = std::string(cache) + "/pipelines.log";
        gJournalBegan = nowUs();
        struct stat journalStat;
        if (stat(gJournalPath.c_str(), &journalStat) == 0 && journalStat.st_size > (2 << 20))
            rename(gJournalPath.c_str(), (gJournalPath + ".old").c_str());
    }
    char name[128] = {};
    nmb_device_name(gDevice, name, sizeof name);
    gName = [NSString stringWithUTF8String:name];
    gRegistryID = registryID;
    // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
    {
        char waived[4096] = {};
        nmb_device_family_waivers(gDevice, waived, sizeof waived);
        for (char *line = waived, *next; *line; line = next) {
            next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            else
                next = line + strlen(line);
            journal(std::string("F ") + line);
        }
    }
    Class base = objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
    // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
    // own _MTLDevice implementation.
    if (getenv("NMB_DEVICE_BASE_NSOBJECT")) base = [NSObject class];
#endif
#if TARGET_OS_IPHONE
    // The target's base class ABI (iPadOS 17.7.11): the dynamic subclass adds no instance variables.
    if (!base || class_getInstanceSize(base) != 536) {
        NBLOG(@"native-metal: unexpected _MTLDevice ABI");
        return nil;
    }
#endif
    if (!base) base = [NSObject class];
    Class cls = objc_allocateClassPair(base, "NativeBridgeMetalDevice", 0);
    if (!cls) return nil;
    unsigned count = 0;
    Method *methods = class_copyMethodList([NBDeviceMethods class], &count);
    for (unsigned i = 0; i < count; i++)
        class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                        method_getTypeEncoding(methods[i]));
    free(methods);
    installAnswers(cls);
    class_addProtocol(cls, @protocol(MTLDevice));
    // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
    // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
    if (Protocol *spi = objc_getProtocol("MTLDeviceSPI")) class_addProtocol(cls, spi);
    objc_registerClassPair(cls);
    gDeviceObject = [cls new];
    fillLimits(gDeviceObject);
    return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes, size_t length)
{
    NmbLibrary *handle = gDevice ? nmb_library_new(gDevice, bytes, length, true) : nullptr;
    if (!handle) return nil;
    NBLibrary *library = [NBLibrary new];
    library.handle = handle;
    return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out)
{
    return gDevice && nmb_device_shader_cache_stats(gDevice, out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10])
{
    return gDevice && nmb_device_work_stats(gDevice, out);
}

extern "C" size_t native_bridge_metal_status(char *out, size_t size)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    return (size_t)snprintf(out, size, "c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",
                            (unsigned long long)gCommits.load(), (unsigned long long)gCommitFailures.load(),
                            (unsigned long long)gCompleted.load(), (unsigned long long)gPipelines.load(),
                            (unsigned long long)gProblems.load(), (unsigned long long)gPasses.load(),
                            (unsigned long long)gDraws.load(), (unsigned long long)gFullScreenPasses.load(),
                            (unsigned long long)(gBiggestPass.load() >> 10), (unsigned long long)gDispatches.load(),
                            (unsigned long long)gBlits.load(), gLastProblems[0], gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out, size_t size)
{
    int n = snprintf(out, size, "tex x%llu f", (unsigned long long)gTextureRefusals.load());
    for (unsigned i = 0; i < gFormatCount && n > 0 && (size_t)n < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%u ", gFormats[i]);
    return n > 0 ? (size_t)n : 0;
}
// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;
extern "C" size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size)
{
    auto now = [] { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); };
    // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
    const unsigned base = gBenchRuns++ ? 2u : 0u;
    auto put = [&](unsigned row, const char *format, auto... args) {
        std::lock_guard<std::mutex> lock(gBenchLock);
        snprintf(gBench[base + row], sizeof gBench[base + row], format, args...);
        if (base) gBench[base + row][0] = 'Z';
    };
    // compute: a dependent integer chain (no memory), 100M steps
    uint64_t t = now(), x = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i < 100000000u; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    const uint64_t computeUs = (now() - t) / 1000;
    // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
    const size_t bytes = 32u << 20;
    uint64_t faultUs = 0, fillUs = 0, shaUs = 0;
    void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != MAP_FAILED) {
        volatile uint8_t *b = (volatile uint8_t *)m;
        t = now();
        for (size_t at = 0; at < bytes; at += 4096) b[at] = 1;
        faultUs = (now() - t) / 1000;
        t = now();
        for (int k = 0; k < 4; k++) memset(m, k, bytes);
        fillUs = (now() - t) / 1000;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        t = now();
        CC_SHA256(m, (CC_LONG)(16u << 20), digest);
        shaUs = (now() - t) / 1000;
        x ^= digest[0];
        munmap(m, bytes);
    }
    put(0, "Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",
        (unsigned long long)(computeUs / 1000), (unsigned long long)faultUs,
        (unsigned long long)(faultUs * 1000 / 8192), (unsigned long long)(fillUs ? 4ull * 32 * 1000000 / fillUs : 0),
        (unsigned long long)(shaUs ? 16ull * 1000000 / shaUs : 0), (unsigned long long)(x & 0xf));
    // pipelines: the host tool's pairs, in its order
    uint64_t made = 0, refused = 0, createUs = 0, createMax = 0, drawUs = 0, drawMax = 0, libraryUs = 0;
    @autoreleasepool {
        id<MTLDevice> dev = (id<MTLDevice>)device;
        NSError *error = nil;
        t = now();
        id<MTLLibrary> qc = metallib ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error] : nil;
        libraryUs = (now() - t) / 1000;
        if (qc) {
            NSArray<NSString *> *names = qc.functionNames;
            id<MTLCommandQueue> queue = [dev newCommandQueue];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [dev newTextureWithDescriptor:td];
            id<MTLBuffer> zeros = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            unsigned n = 0;
            for (NSString *name in names) {
                if (n >= pairs) break;
                id<MTLFunction> f = [qc newFunctionWithName:name];
                if (f.functionType != MTLFunctionTypeVertex) continue;
                NSString *fragmentName = [name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
                if ([fragmentName isEqualToString:name] || ![names containsObject:fragmentName]) continue;
                ++n;
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = f;
                d.fragmentFunction = [qc newFunctionWithName:fragmentName];
                d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                t = now();
                id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:d error:&error];
                const uint64_t took = (now() - t) / 1000;
                if (!state) {
                    ++refused;
                    continue;
                }
                ++made;
                createUs += took;
                createMax = std::max(createMax, took);
                t = now();
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:state];
                for (NSUInteger i = 0; i < 8; ++i) {
                    [encoder setVertexBuffer:zeros offset:0 atIndex:i];
                    [encoder setFragmentBuffer:zeros offset:0 atIndex:i];
                }
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                [commands commit];
                [commands waitUntilCompleted];
                const uint64_t drew = (now() - t) / 1000;
                drawUs += drew;
                drawMax = std::max(drawMax, drew);
            }
        }
    }
    put(1, "Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",
        (unsigned long long)(libraryUs / 1000), (unsigned long long)made, (unsigned long long)refused,
        (unsigned long long)(createUs / 1000), (unsigned long long)(createMax / 1000),
        (unsigned long long)(drawUs / 1000), (unsigned long long)(drawMax / 1000));
    size_t at = 0;
    if (out && size) {
        std::lock_guard<std::mutex> lock(gBenchLock);
        out[0] = 0;
        for (unsigned r = base; r < base + 2; r++)
            at += (size_t)snprintf(out + at, size > at ? size - at : 0, "%s\n", gBench[r]);
    }
    return at;
}
// One line of rates since the previous call (the plugin logs it every 5 s; 0 and nothing written when no command buffer was
// committed and no pipeline made since): commits per second; process CPU per commit and mean encoding wall time; the runtime's
// commit call (submission, with any CPU wait in it); time blocked in waitUntilCompleted/Scheduled and resource waits; GPU latency
// (commit to completion); per commit the draws, the render passes and the time inside this frontend's encoding calls (api: the
// rest of enc is the caller's own work between them), and the passes that could have joined the one before (join); the runtime's
// host calls that waited for a submission of their own (staging queue, internal) and its host texel copies (gw); pipeline states
// made (count, ms) and the driver's VkPipeline makings (count, ms); the shader cache's hits, misses and writes, and the driver
// records' hits and misses; the host imports the GPU wrote that this process read back, invalidated lazily (rb: count/MB/ms); last,
// surface imports made (count/ms) and answered by a live one (hits), and textures made over them (stx: count/ms).
extern "C" size_t native_bridge_metal_rates(char *out, size_t size)
{
    // sys= is the kernel's part of cpu= (both per commit, all of this process's threads), flt= the process's page faults per commit.
    struct Snapshot {
        uint64_t at, cpu, sys, faults, commits, encode, submitN, submitUs, waitN, waitUs, gpuN, gpuUs, draws, passes,
            apiUs, joinable, psoN, psoUs, texN, texUs, asked, kept, planar, rb, rbBytes, rbUs, impN, impUs, impHits,
            stxN, stxUs;
        uint64_t work[10], counts[10], waits[4];
        NmbShaderCacheStats cache;
    };
    static std::mutex lock;
    static Snapshot last{};
    Snapshot now{};
    now.at = nowUs();
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    now.cpu = (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec +
              usage.ru_stime.tv_usec;
    now.sys = (uint64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
    now.faults = (uint64_t)(usage.ru_minflt + usage.ru_majflt);
    now.commits = gCommits.load();
    now.encode = gEncodeUs.load();
    now.submitN = gTimeSubmit.count.load();
    now.submitUs = gTimeSubmit.total.load();
    now.waitN = gTimeWaitCompleted.count.load() + gTimeWaitScheduled.count.load() + gTimeResourceWait.count.load();
    now.waitUs = gTimeWaitCompleted.total.load() + gTimeWaitScheduled.total.load() + gTimeResourceWait.total.load();
    now.gpuN = gTimeEncodeToCommit.count.load();
    now.gpuUs = gTimeEncodeToCommit.total.load();
    now.draws = gDraws.load();
    now.passes = gPasses.load();
    now.apiUs = gApiUs.load();
    now.joinable = gJoinablePasses.load();
    now.psoN = gTimePipeline.count.load();
    now.psoUs = gTimePipeline.total.load();
    now.texN = gTimeTexelCopy.count.load();
    now.texUs = gTimeTexelCopy.total.load();
    now.asked = gAnswersAsked.load();
    now.kept = gAnswersKept.load();
    now.planar = gSurfaceMultiPlane.load();
    now.rb = gReadbackInvalidates.load();
    now.rbBytes = gReadbackBytes.load();
    now.rbUs = gReadbackUs.load();
    now.impN = gTimeSurfaceImport.count.load();
    now.impUs = gTimeSurfaceImport.total.load();
    now.impHits = gSurfaceImportHits.load();
    now.stxN = gTimeSurfaceTexture.count.load();
    now.stxUs = gTimeSurfaceTexture.total.load();
    if (gDevice) {
        nmb_device_work_stats(gDevice, now.work);
        nmb_device_shader_cache_stats(gDevice, &now.cache);
        nmb_device_counts(gDevice, now.counts);
        nmb_device_wait_counts(gDevice, now.waits);
    }
    std::lock_guard<std::mutex> guard(lock);
    const Snapshot was = last;
    last = now;
    const uint64_t commits = now.commits - was.commits, pso = now.psoN - was.psoN, vk = now.work[0] - was.work[0];
    // r6-lows: this window's command-buffer distributions, taken (and cleared) whether or not the line is written.
    const NBHist::Taken enc = gHistEncode.take(17000, 34000), gap = gHistGap.take(25000, 34000),
                        sched = gHistSched.take(17000, 34000);
    const NBHist::Taken gpu = gHistGpu.take(17000, 34000), made = gHistPso.take(17000, 34000);
    if (!was.at || (!commits && !pso && !vk)) {
        if (out && size) out[0] = 0;
        return 0;
    }
    auto per = [](uint64_t total, uint64_t n) { return n ? (double)total / n / 1000.0 : 0.0; };  // ms per item
    const double seconds = (double)(now.at - was.at) / 1e6;
    const double perCommit = commits ? 1.0 / (double)commits : 0.0;
    const int n = snprintf(
        out, size,
        "%.0fs c=%llu(%.1f/s) cpu=%.1fms/c sys=%.1fms flt=%.0f enc=%.1fms sub=%.2fms wait=%llums/%llu tex=%llu/%llums "
        "gpu=%.1fms "
        "d=%.0f p=%.1f api=%.1fms join=%llu gw=s%llu/i%llu/h%llu pso=%llu/%llums vk=%llu/%llums "
        "cache=h%llu/m%llu/w%llu drv=l%llu/h%llu/m%llu ans=%llu/%llu planar=%llu mem=%lluMB/%llu+%llu+%llu tlw=%llu "
        "var=%llu lnk=%llu fsl=%llu shadow=%llu rb=%llu/%lluMB/%.1fms imp=%llu/%.1fms/h%llu stx=%llu/%.1fms",
        seconds, (unsigned long long)commits, commits / seconds, per(now.cpu - was.cpu, commits),
        per(now.sys - was.sys, commits), (double)(now.faults - was.faults) * perCommit,
        per(now.encode - was.encode, commits), per(now.submitUs - was.submitUs, now.submitN - was.submitN),
        (unsigned long long)((now.waitUs - was.waitUs) / 1000), (unsigned long long)(now.waitN - was.waitN),
        (unsigned long long)(now.texN - was.texN), (unsigned long long)((now.texUs - was.texUs) / 1000),
        per(now.gpuUs - was.gpuUs, now.gpuN - was.gpuN), (double)(now.draws - was.draws) * perCommit,
        (double)(now.passes - was.passes) * perCommit, per(now.apiUs - was.apiUs, commits),
        (unsigned long long)(now.joinable - was.joinable), (unsigned long long)(now.waits[0] - was.waits[0]),
        (unsigned long long)(now.waits[1] - was.waits[1]), (unsigned long long)(now.waits[2] - was.waits[2]),
        (unsigned long long)pso, (unsigned long long)((now.psoUs - was.psoUs) / 1000), (unsigned long long)vk,
        (unsigned long long)((now.work[1] - was.work[1]) / 1000), (unsigned long long)(now.work[7] - was.work[7]),
        (unsigned long long)(now.work[8] - was.work[8]), (unsigned long long)(now.work[9] - was.work[9]),
        (unsigned long long)(now.cache.driver_loads - was.cache.driver_loads),
        (unsigned long long)(now.cache.driver_hits - was.cache.driver_hits),
        (unsigned long long)(now.cache.driver_misses - was.cache.driver_misses),
        (unsigned long long)(now.asked - was.asked), (unsigned long long)(now.kept - was.kept),
        (unsigned long long)(now.planar - was.planar), (unsigned long long)(now.counts[0] >> 20),
        (unsigned long long)now.counts[1], (unsigned long long)now.counts[2], (unsigned long long)now.counts[3],
        (unsigned long long)(now.counts[4] - was.counts[4]), (unsigned long long)(now.counts[5] - was.counts[5]),
        (unsigned long long)(now.counts[6] - was.counts[6]), (unsigned long long)(now.counts[7] - was.counts[7]),
        (unsigned long long)(now.counts[8] - was.counts[8]), (unsigned long long)(now.rb - was.rb),
        (unsigned long long)((now.rbBytes - was.rbBytes) >> 20), (double)(now.rbUs - was.rbUs) / 1000.0,
        (unsigned long long)(now.impN - was.impN), (double)(now.impUs - was.impUs) / 1000.0,
        (unsigned long long)(now.impHits - was.impHits), (unsigned long long)(now.stxN - was.stxN),
        (double)(now.stxUs - was.stxUs) / 1000.0);
    size_t at = n > 0 ? std::min<size_t>((size_t)n, size ? size - 1 : 0) : 0;
    // r6-lows, in ms: encp and cgap (one thread's commit to its next) p50/p99/max with the counts at 17 ms and over (past a 60 Hz
    // frame) and, for cgap, at 25 and 34 ms and over (1.5x and over 2x the frame); swait (waitUntilScheduled) and gpuq (commit to
    // completion) p50/p99/max; the slowest render pipeline state made. Bins are whole ms.
    auto ms = [](uint64_t us) { return (unsigned long long)(us / 1000); };
    if (at + 1 < size) {
        const int more =
            snprintf(out + at, size - at,
                     " encp=%llu/%llu/%llums>16:%llu cgap=%llu/%llu/%llums>25:%llu>33:%llu swait=%llu/%llu/%llums "
                     "gpuq=%llu/%llu/%llums psomax=%llums",
                     ms(enc.p50), ms(enc.p99), ms(enc.max), (unsigned long long)enc.over, ms(gap.p50), ms(gap.p99),
                     ms(gap.max), (unsigned long long)gap.over, (unsigned long long)gap.over2, ms(sched.p50),
                     ms(sched.p99), ms(sched.max), ms(gpu.p50), ms(gpu.p99), ms(gpu.max), ms(made.max));
        if (more > 0) at = std::min<size_t>(at + (size_t)more, size - 1);
    }
    return at;
}

// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;
    auto row = [&](const char *format, auto... args) {
        char text[200];
        int n = snprintf(text, sizeof text, format, args...);
        if (n <= 0) return;
        if (n > 90) n = 90;
        if (at + (size_t)n + 2 >= size) return;
        memcpy(out + at, text, (size_t)n);
        at += (size_t)n;
        out[at++] = '\n';
        out[at] = 0;
    };
    auto ms = [](uint64_t us) { return (unsigned long long)(us / 1000); };
    {
        std::lock_guard<std::mutex> lock(gBenchLock);
        for (unsigned r = 0; r < 4; r++)
            if (gBench[r][0]) row("%.90s", gBench[r]);
    }
    row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu", (unsigned long long)gCommits.load(),
        (unsigned long long)gCommitFailures.load(), (unsigned long long)gCompleted.load(),
        (unsigned long long)gPipelines.load(), (unsigned long long)gProblems.load(), (unsigned long long)gPasses.load(),
        (unsigned long long)gDraws.load(), (unsigned long long)gFullScreenPasses.load(),
        (unsigned long long)(gBiggestPass.load() >> 10), (unsigned long long)gDispatches.load(),
        (unsigned long long)gBlits.load());
    row("T pipeline n=%llu total=%llums max=%llums", (unsigned long long)gTimePipeline.count.load(),
        ms(gTimePipeline.total.load()), ms(gTimePipeline.slowest.load()));
    row("T library n=%llu total=%llums max=%llums", (unsigned long long)gTimeLibrary.count.load(),
        ms(gTimeLibrary.total.load()), ms(gTimeLibrary.slowest.load()));
    row("T function n=%llu total=%llums max=%llums", (unsigned long long)gTimeFunction.count.load(),
        ms(gTimeFunction.total.load()), ms(gTimeFunction.slowest.load()));
    row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",
        (unsigned long long)gTimeEncodeToCommit.count.load(), ms(gTimeEncodeToCommit.total.load()),
        ms(gTimeEncodeToCommit.slowest.load()),
        (unsigned long long)(gTimeEncodeToCommit.count.load()
                                 ? gTimeEncodeToCommit.total.load() / gTimeEncodeToCommit.count.load()
                                 : 0));
    row("J scheduled wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeWaitScheduled.count.load(),
        ms(gTimeWaitScheduled.total.load()), ms(gTimeWaitScheduled.slowest.load()));
    row("J completed wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeWaitCompleted.count.load(),
        ms(gTimeWaitCompleted.total.load()), ms(gTimeWaitCompleted.slowest.load()));
    row("J resource wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeResourceWait.count.load(),
        ms(gTimeResourceWait.total.load()), ms(gTimeResourceWait.slowest.load()));
    row("J resource checks=%llu skipped=%llu", (unsigned long long)gResourceChecks.load(),
        (unsigned long long)gResourceSkipped.load());
    // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
    // its shader cache: where the driver's compile time goes.
    uint64_t work[10] = {};
    if (gDevice && nmb_device_work_stats(gDevice, work)) {
        row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu", (unsigned long long)work[0],
            ms(work[1]), ms(work[2]), ms(work[3]), (unsigned long long)work[4]);
        row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu", (unsigned long long)work[7],
            (unsigned long long)work[8], (unsigned long long)work[9], (unsigned long long)work[5],
            (unsigned long long)work[6]);
        char warning[200];
        size_t n = nmb_last_warning(warning, sizeof warning);
        if (n) {
            row("W %.86s", warning);
            if (n > 86) row("Wb %.86s", warning + 86);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gProblemLock);
        char formats[160];
        native_bridge_metal_formats_locked(formats, sizeof formats);
        row("F %.86s", formats);
        row("P0 %.86s", gLastProblems[0]);
        row("P0b %.86s", strlen(gLastProblems[0]) > 86 ? gLastProblems[0] + 86 : "");
        row("P1 %.86s", gLastProblems[1]);
        row("P1b %.86s", strlen(gLastProblems[1]) > 86 ? gLastProblems[1] + 86 : "");
    }
    {
        std::lock_guard<std::mutex> lock(gCommitLock);
        const uint32_t newest = (uint32_t)gCommits.load();
        row("%s", "Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
        for (uint32_t k = 0; k < 20 && k < newest; k++) {
            const CommitRecord &r = gCommitRing[(newest - k) % 32];
            if (r.index != newest - k) continue;
            row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus", r.index,
                (unsigned long long)(r.committedUs / 1000000), (unsigned long long)((r.committedUs / 1000) % 1000),
                r.work.passes, r.work.draws, (unsigned long long)(r.work.pixels >> 10), r.work.dispatches, r.work.blits,
                (unsigned long long)r.callbackUs);
            if (k < 4)
                row("E%u age=%lluus scheduled=%lldus", r.index, (unsigned long long)r.ageUs, (long long)r.scheduledUs);
        }
    }
    return at;
}

extern "C" size_t native_bridge_metal_formats(char *out, size_t size)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    return native_bridge_metal_formats_locked(out, size);
}
