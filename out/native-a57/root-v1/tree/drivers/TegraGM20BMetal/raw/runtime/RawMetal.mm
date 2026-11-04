// SPDX-License-Identifier: MIT
// See RawMetal.h. r7-raw wrote the first facade (render encoder, pipeline key); r8-rawca made it the render server's device: the
// m2v twins, the recorded command buffer and its replay on m2v, the order between the two drivers, the copies, the events,
// the lifetimes.
//
// A command buffer here is a tape: each encoder call appends a record (the bytes of setBytes copied in, every object retained by
// the command buffer until it completes). A call the raw path cannot draw exactly as m2v marks the command buffer for m2v, with a
// reason. At commit, under the device's submit lock and in commit order:
//  - raw: the tape is encoded into the raw rings (raw_gpu.cpp) and submitted, one Exec. The encoder can still refuse a draw (a
//    uniform buffer at an unaligned offset, a texture without a header); the raw work is then abandoned (the rings rewound)
//    and the command buffer goes to m2v. The completion thread sleeps in the kernel on each fence (the interrupt), then
//    runs the handlers.
//  - m2v: the tape is replayed into a command buffer of the m2v device with every object's twin, CPU lines of the buffers it
//    reads cleaned, and committed once the raw work committed before it has completed (raw's fence, a kernel wait on the
//    interrupt). Its completion handler runs ours.
//  - a raw command buffer waits for the m2v ones committed before it, and for the shared events it waits on: such a command
//    buffer, and every one committed after it while it waits, goes to the submit thread, which submits them in order once
//    they may run; the render thread never blocks for it. waitUntilScheduled waits for the submission, so the display's
//    copy (which waits on the GPU for every channel that imports the surface) never runs before the frame's work is submitted.
#import "RawMetal.h"
#import <IOSurface/IOSurfaceRef.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "raw_gpu.h"

// The m2v frontend's in-place texture over a buffer (NativeBridgeMetalFrontend.mm): weak, so the host benchmark links without it.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow)
    __attribute__((weak_import));

static os_log_t rawLog()
{
    static os_log_t l = os_log_create("com.ioswitch.TegraGM20BMetal", "raw");
    return l;
}

// A wait the plugin bounds (r9-hang: r7n's and r7o's freezes were a userland wait with no end in backboardd's render thread):
// past kWaitLimit, one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze, the userland twin of the kernel's missing-interrupt panic. Nothing waits that long when it works: the kernel's own GPU
// waits panic within 10 s.
static constexpr auto kWaitLimit = std::chrono::seconds(20);
[[noreturn]] static void waitTooLong(const char *what, const void *object)
{
    os_log_fault(rawLog(), "raw: %{public}s %p not done after %lld s: abort() for the stack", what, object,
                 (long long)kWaitLimit.count());
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s %p not done after %lld s\n", what, object, (long long)kWaitLimit.count());
#endif
    abort();
}

// How long waitUntilScheduled waits before it asks whether a shared event holds the command buffer (waitUntilScheduled).
static constexpr auto kHeldGrace = std::chrono::milliseconds(500);

// One line per distinct text, then silence (shipped paths: no diagnostics but one-shot lines).
static void once(NSString *text)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> l(lock);
        if (seen.size() > 512 || !seen.insert(text.UTF8String).second) return;
    }
    os_log_error(rawLog(), "raw: %{public}@", text);
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s\n", text.UTF8String);  // host tests read their stderr
#endif
}

static double hostSeconds()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return double(mach_absolute_time()) * tb.numer / tb.denom / 1e9;
}
static uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

// ---- refusal diagnostics (r10-rawsweep): which predicate sends work to m2v, with its values ----
// Every refusal site names its predicate. A command buffer keeps a mask of every predicate it hit (not only the first, which
// decides its reason) and the first one's values. At submission each predicate hit is counted once per command buffer
// (gPredCbs), and once more when it was the command buffer's only one (gPredOnly: what fixing it alone would gain); the 5 s rates
// line shows both. The first hit of each predicate in the process logs one line with its values (and, for a resource refused
// when made, the resource's details). Hot path: an OR into the mask and a relaxed load; strings only on a predicate's first hit.
enum RMPred : uint8_t {
    PredNone = 0,
    // textures the raw path does not keep, decided when made (a bind hits the texture's own predicate)
    PredTexShape,  // rawShape: a = failing term (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format), b = format
    PredTexPoolAlloc,   // no pool span or texture header
    PredTexSurfPlane,   // a plane of a planar surface (not the 4:4:4 one): a = plane, b = plane count
    PredTexSurfPitch,   // bytesPerRow & 31, or short: a = bytesPerRow, b = width x bpp
    PredTexSurfBounds,  // offset + rows past the allocation
    PredTexSurfBase,    // base address off a page: a = base & 0xfff
    PredTexSurfImport,  // the import or the header slot refused
    PredTexPlanar,      // the 4:4:4 two-plane texture refused
    PredTexBufBase,     // (va + offset) & 255: a = that, b = offset
    PredTexBufPitch,    // bytesPerRow & 31, or short: a = bytesPerRow, b = width x bpp
    PredTexBufOther,    // memoryless, past the buffer, header slot
    PredTexView,  // a view of a texture the raw path does not keep, or of another texel size: a = format, b = parent raw
    PredTexForeign,  // not an RMTexture (m2v's own: over an m2v buffer, or a creation selector the facade does not answer)
    // passes
    PredPassAttFormat,  // a color attachment not kept or not renderable: a = format, b = attachment index | kind << 8
    PredPassAttLevel,   // level, slice, depth plane or resolve
    PredPassRTSize,     // a = renderTargetWidth << 16 | renderTargetHeight, b = attachment width << 16 | height
    PredPassDepthStencil,  // a = depth format, b = stencil format
    PredPassOther,         // visibility buffer, layered target, rasterization rate map
    // states and calls
    PredStateForeign,
    PredStateNotQuartzCore,  // a state whose functions are not QuartzCore's default.metallib (never looked up in the pack)
    PredStateNotInPack,
    PredStateConstBlend,
    PredCallVisibility,
    PredCallIndexedForm,     // a = instances, b = baseVertex | baseInstance << 32
    PredCallBufForeign,      // a = stage << 8 | index
    PredCallSamplerForeign,  // a sampler the raw path does not express (its reason logged when it was made)
    PredCallVertexSampler,
    PredCallCull,
    PredCallFillMode,
    PredCallInvocation,    // an encoder selector recorded as an invocation (each selector logged once)
    PredCallCBInvocation,  // a command buffer selector recorded as an invocation
    PredCallBlitOptions,
    PredCallBufCopyRange,
    PredBlitTexCopy,
    PredBlitBufToTex,
    PredBlitTexToBuf,
    PredBlitWholeTexCopy,
    PredCompute,
    PredMipmaps,
    PredEvent,
    // found while encoding (the raw encode ran up to here and was thrown away)
    PredEncPlanarCopy,
    PredEncPlanarClamp,
    PredEncPlanarRT,
    PredEncPassRender,
    PredEncBufToTexRow,  // a = sourceBytesPerRow
    PredEncKernel,
    PredEncIndexedRange,  // a = offset, b = index size
    PredEncBaseInstance,
    PredEncColorFormats,  // a = attachment, b = pass format, c in the line: state format
    PredEncUboOffset,     // a = stage << 8 | slot, b = va & 255
    PredEncNoTic,
    PredCount
};
static_assert(PredCount <= 64, "one mask word");
static const char *const kPredNames[] = {"-",
                                         "tex.shape",
                                         "tex.pool",
                                         "tex.surf.plane",
                                         "tex.surf.pitch",
                                         "tex.surf.bounds",
                                         "tex.surf.base",
                                         "tex.surf.imp",
                                         "tex.planar",
                                         "tex.buf.base",
                                         "tex.buf.pitch",
                                         "tex.buf.other",
                                         "tex.view",
                                         "tex.foreign",
                                         "pass.fmt",
                                         "pass.level",
                                         "pass.rtsize",
                                         "pass.ds",
                                         "pass.other",
                                         "state.foreign",
                                         "state.notqc",
                                         "state.pack",
                                         "state.cblend",
                                         "call.vis",
                                         "call.idxform",
                                         "call.buf",
                                         "call.sampler",
                                         "call.vsampler",
                                         "call.cull",
                                         "call.fill",
                                         "call.inv",
                                         "call.cbinv",
                                         "call.blitopt",
                                         "call.bufcopy",
                                         "blit.tex",
                                         "blit.b2t",
                                         "blit.t2b",
                                         "blit.whole",
                                         "compute",
                                         "mipmaps",
                                         "event",
                                         "enc.planarcopy",
                                         "enc.planarclamp",
                                         "enc.planarrt",
                                         "enc.pass",
                                         "enc.b2trow",
                                         "enc.kernel",
                                         "enc.idxrange",
                                         "enc.baseinst",
                                         "enc.colorfmt",
                                         "enc.ubo",
                                         "enc.notic"};
static_assert(sizeof kPredNames / sizeof *kPredNames == PredCount, "a name for each predicate");
static std::atomic<uint64_t> gPredCbs[PredCount], gPredOnly[PredCount], gPredLogged{0};
static uint64_t gPredRates[PredCount];  // the rates line's last gPredCbs (its timer thread only)

static inline bool predLogged(RMPred p)
{
    return gPredLogged.load(std::memory_order_relaxed) >> p & 1;
}
// The predicate's first hit in the process: one line with its values (`detail`: the resource's, when it was made)
static void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail)
{
    const uint64_t bit = 1ull << p;
    if (gPredLogged.fetch_or(bit, std::memory_order_relaxed) & bit) return;
    os_log_error(rawLog(), "raw: first %{public}s: a=%llu (0x%llx) b=%llu (0x%llx)%{public}s%{public}@", kPredNames[p],
                 (unsigned long long)a, (unsigned long long)a, (unsigned long long)b, (unsigned long long)b,
                 detail ? " " : "", detail ?: @"");
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: first %s: a=%llu b=%llu %s\n", kPredNames[p], (unsigned long long)a, (unsigned long long)b,
            detail ? detail.UTF8String : "");
#endif
}

// The type encoding of a selector some MTL* protocol declares (the plugin's answerUnimplemented uses the same lookup).
static const char *metalTypes(SEL selector)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *protocols = objc_copyProtocolList(&count);
    const char *types = nullptr;
    for (unsigned i = 0; i < count && !types; ++i) {
        if (strncmp(protocol_getName(protocols[i]), "MTL", 3) != 0) continue;
        for (int required = 1; required >= 0 && !types; --required) {
            const objc_method_description d = protocol_getMethodDescription(protocols[i], selector, required, YES);
            if (d.name) types = d.types;
        }
    }
    free(protocols);
    return types;
}

// A texture the raw path keeps: 2D, one level, one sample, a format it has a header for, not rotated (the descriptor's rotation SPI:
// Core Animation's window-server surfaces on a rotated display, CA::OGL::MetalContext::get_surface; m2v renders those rotated)
static bool rawShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned && raw::formatInfo(uint32_t(d.pixelFormat));
}
// Diagnostics: which rawShape term fails (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format; 0 none)
static uint32_t rawShapeTerm(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType != MTLTextureType2D           ? 1
           : d.mipmapLevelCount > 1                    ? 2
           : d.sampleCount > 1                         ? 3
           : (d.arrayLength > 1 || d.depth > 1)        ? 4
           : turned                                    ? 5
           : !raw::formatInfo(uint32_t(d.pixelFormat)) ? 6
                                                       : 0;
}
static NSString *describe(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return [NSString
        stringWithFormat:@"%lux%lu format %lu type %lu levels %lu samples %lu array %lu rotation %lu usage 0x%lx "
                         @"storage %lu",
                         (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.arrayLength, (unsigned long)turned, (unsigned long)d.usage,
                         (unsigned long)d.storageMode];
}
// A texture the raw path does not keep: its predicate, and the predicate's first line with the texture's details
@class RMTexture;
static void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra);

// MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes)
static size_t constantBytes(uint64_t type)
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

@protocol RMConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (RMPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

@class RMDevice, RMCommandBuffer;

// The process's one raw device (the render server's): its state. The device object Core Animation holds is an instance of a
// subclass of the m2v device's own class (raw_metal_device_open), so it is an _MTLDevice that conforms to Metal's protocols as
// the m2v device does and answers every selector the facade does not override with the m2v device's own method; it has no
// instance variables of its own (the base class's ABI is fixed), so its state lives here.
// What makes a command buffer go to m2v (counted, one log line each kind)
enum RMReason : uint8_t {
    WhyRaw = 0,
    WhyState,
    WhyTexture,
    WhyPass,
    WhyCall,
    WhyCompute,
    WhyMipmaps,
    WhyEvent,
    WhyEncode,
    WhyCount
};
static const char *const kReasonNames[WhyCount] = {"raw",     "state",   "texture", "pass",  "call",
                                                   "compute", "mipmaps", "event",   "encode"};
struct Stats {
    std::atomic<uint64_t> raw{0}, m2v{0}, deferred{0}, aborted{0}, encodeUs{0}, draws{0}, passes{0}, copies{0},
        reasons[WhyCount], switches{0}, cpuOrdered{0}, acquires{0}, eventsReleased{0}, heldReturns{0};
    // diagnostics (r10-rawsweep): the raw encode thrown away by aborted command buffers, the m2v replay's time on the committing
    // thread (replay + commit), and the bytes replay cleans
    std::atomic<uint64_t> abortUs{0}, replayUs{0}, replayClean{0};
};
struct RawState {
    raw::Device *raw = nullptr;
    id<MTLDevice> fallback;                 // the m2v device
    id device;                              // the raw device object
    std::mutex submit;                      // one command buffer encodes and submits at a time, in commit order
    std::deque<RMCommandBuffer *> waiting;  // committed, not yet submitted
    std::condition_variable waitingChanged;
    std::mutex m2vLock;
    std::condition_variable m2vChanged;
    uint32_t m2vPending = 0;      // m2v command buffers committed and not completed
    uint32_t m2vUnscheduled = 0;  // and not scheduled yet: their kernel submission (NVK's Exec) is not made
    // The two drivers' order (submit lock): on the GPU when the kernel has Order (TegraGM20BInfoOrder), else on the CPU (the
    // m2v work before a raw command buffer completes first, and the raw work before an m2v one).
    bool gpuOrder = false;
    bool rawSinceM2v =
        false;  // raw work submitted since the last m2v commit: the next m2v one waits for it (BeforeTask)
    bool m2vSinceRaw =
        false;           // m2v work submitted since the last raw submission: the next raw one waits for it (AfterTask)
    int lastDriver = 0;  // 1 raw, 2 m2v: the last command buffer's, for the switch count
    NSMapTable *rawSignals,
        *m2vSignals;  // shared event -> highest value a submitted command buffer of each driver signals
    MTLSharedEventListener *listener;  // the CPU-ordered remainder's event waits
    std::mutex doneLock;
    std::condition_variable doneChanged;
    std::deque<RMCommandBuffer *> inflight;  // raw command buffers submitted, in seqno order
    dispatch_queue_t handlers;               // where scheduled handlers run (in order, never inside commit)
    Stats stats;
};
static RawState *gRM;
static uint64_t signalled(NSMapTable *map, void *event);  // the order section's (below)

// ---- the common base: an m2v twin and the selectors this facade does not answer itself ----
@interface RMObject : NSObject {
@public
    __weak id _device;
    id _twin;  // the m2v object over the same memory (made on first need)
}
@property (nonatomic, copy) NSString *label;
- (id)rmTwin;  // made now if needed; nil if it cannot be
@end

@interface RMDevice : NSObject  // the methods the device class gets (raw_metal_device_open); no state of its own
- (void)rmCommit:(RMCommandBuffer *)cb;
@end

@implementation RMObject
- (id)rmTwin
{
    return _twin;
}
- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}
// A selector this class does not answer goes to the twin (an m2v object: its answer is today's). respondsToSelector: says what the
// twin's class says, so Core Animation's optional paths choose as they do on m2v.
- (id)forwardingTargetForSelector:(SEL)selector
{
    id twin = [self rmTwin];
    if (!twin)
        once([NSString stringWithFormat:@"-[%s %s] with no m2v twin", class_getName(object_getClass(self)),
                                        sel_getName(selector)]);
    return twin;
}
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector]) return YES;
    if (_twin) return [_twin respondsToSelector:selector];
    Class c = [[self class] twinClass];
    return c ? [c instancesRespondToSelector:selector] : NO;
}
+ (Class)twinClass
{
    return nil;
}
@end

// ---- functions and libraries: the m2v library and functions, with the name and constants text the pack keys by ----
@interface RMLibrary : RMObject {
@public
    bool _quartzCore;  // QuartzCore's default.metallib: the only library the pack's functions come from
}
@end
@interface RMFunction : RMObject {
@public
    std::string _constants;
    bool _quartzCore;
}
@property (nonatomic, copy) NSString *name;
@end
@implementation RMFunction
+ (Class)twinClass
{
    return NSClassFromString(@"NBFunction");
}
- (MTLFunctionType)functionType
{
    return [(id<MTLFunction>)_twin functionType];
}
@end

static std::string constantsText(MTLFunctionConstantValues *values)
{
    std::string text;
    if (!values) return text;
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *groups[2] = {[snapshot newIndexedConstantArray], [snapshot newNamedConstantArray]};
    for (int g = 0; g < 2; g++)
        for (id<RMConstantValue> v in groups[g]) {
            const size_t n = constantBytes(v.dataType);
            char head[48];
            if (g)
                text += std::string(text.empty() ? "" : " ") + v.name.UTF8String;
            else {
                snprintf(head, sizeof head, "%s#%lu", text.empty() ? "" : " ", (unsigned long)v.index);
                text += head;
            }
            snprintf(head, sizeof head, ":%lu:", (unsigned long)v.dataType);
            text += head;
            for (size_t i = 0; i < n; i++) {
                snprintf(head, sizeof head, "%02x", ((const uint8_t *)v.data)[i]);
                text += head;
            }
        }
    return text;
}

@implementation RMLibrary
+ (Class)twinClass
{
    return NSClassFromString(@"NBLibrary");
}
- (RMFunction *)wrap:(id<MTLFunction>)twin name:(NSString *)name constants:(MTLFunctionConstantValues *)values
{
    if (!twin) return nil;
    RMFunction *f = [RMFunction new];
    f->_device = _device;
    f->_twin = twin;
    f->_constants = constantsText(values);
    f->_quartzCore = _quartzCore;
    f.name = name;
    return f;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name] name:name constants:nil];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name
                                                                   constantValues:values
                                                                            error:error]
                                  name:name
                             constants:values];
}
- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    RMLibrary *me = self;
    MTLFunctionConstantValues *copy = [values copy];
    [(id<MTLLibrary>)_twin newFunctionWithName:name
                                constantValues:values
                             completionHandler:^(id<MTLFunction> f, NSError *e) {
                                 handler((id<MTLFunction>)[me wrap:f name:name constants:copy], e);
                             }];
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithDescriptor:d error:error]
                                  name:d.name
                             constants:d.constantValues];
}
@end

static RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path)
{
    if (!twin) return nil;
    RMLibrary *l = [RMLibrary new];
    l->_device = device;
    l->_twin = twin;
    l->_quartzCore = [path hasSuffix:@"QuartzCore.framework/default.metallib"];
    return l;
}

NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *d)
{
    // the journal's R-line fields in its order (NativeBridgeMetalFrontend.mm journalRenderPipeline), but the label, each with a
    // blank after it, then the functions; no trailing blank (the pack's keys: pack/probelog.py)
    char head[160];
    snprintf(head, sizeof head, "samples=%lu topo=%lu ds=%lu/%lu ", (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string key = head;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid) continue;
        snprintf(head, sizeof head, "c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        key += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            snprintf(head, sizeof head, "a%u=%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, (unsigned long)l.stride,
                     (unsigned long)l.stepFunction, (unsigned long)l.stepRate);
            key += head;
        }
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    key += " | v " + std::string(vf ? vf.name.UTF8String : "-") + " " + (vf ? vf->_constants : "");
    key += " | f " + std::string(ff ? ff.name.UTF8String : "-") + " " + (ff ? ff->_constants : "");
    while (!key.empty() && key.back() == ' ') key.pop_back();
    return [NSString stringWithUTF8String:key.c_str()];
}

// ---- resources ----
@interface RMHeap : RMObject {
@public
    MTLHeapDescriptor *_descriptor;
    std::atomic<uint64_t> _used;
}
@end

@interface RMBuffer : RMObject {
@public
    raw::Buffer _raw;
    MTLResourceOptions _options;
    std::atomic<bool> _gpuDirty;  // the GPU wrote it since the CPU last read it
    void (^_deallocator)(void *, NSUInteger);
    RMHeap *_heap;
    MTLPurgeableState _purgeable;
}
@end
@interface RMBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row;
@end

@implementation RMBuffer
+ (Class)twinClass
{
    return NSClassFromString(@"NBBuffer");
}
- (void)dealloc
{
    _twin = nil;  // m2v's import over these pages goes first
    void *bytes = _raw.cpu;
    const NSUInteger length = NSUInteger(_raw.length);
    if (gRM) gRM->raw->destroyBuffer(_raw);
    if (_heap) _heap->_used -= length;
    if (_deallocator) _deallocator(bytes, length);
}
- (id)rmTwin
{
    @synchronized(self) {
        if (!_twin) {
            const size_t page = size_t(getpagesize());
            // over the same pages: a pool span or the caller's no-copy pages, whole pages
            _twin = [gRM->fallback newBufferWithBytesNoCopy:_raw.cpu
                                                     length:(NSUInteger(_raw.length) + page - 1) & ~(page - 1)
                                                    options:_options
                                                deallocator:nil];
        }
        return _twin;
    }
}
// A CPU read after a GPU write: the GPU's bytes, not lines the CPU cached before (clean+invalidate keeps any CPU write)
- (void)invalidateForCpu
{
    if (!_gpuDirty.exchange(false)) return;
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63); a < uintptr_t(_raw.cpu) + _raw.length; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
- (void *)contents
{
    [self invalidateForCpu];
    return _raw.cpu;
}
- (NSUInteger)length
{
    return NSUInteger(_raw.length);
}
- (NSUInteger)allocatedSize
{
    return NSUInteger(_raw.span.valid() ? _raw.span.bytes : _raw.length);
}
- (uint64_t)gpuAddress
{
    return _raw.va;
}
- (MTLResourceOptions)resourceOptions
{
    return _options;
}
- (MTLStorageMode)storageMode
{
    return MTLStorageMode((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheMode((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return MTLHazardTrackingMode((_options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift);
}
- (void)didModifyRange:(NSRange)range
{
    (void)range;  // the draws that read it clean what they read (raw_gpu.cpp CommandBuffer::clean)
}
// The pages are never given back while the buffer lives: Volatile and Empty keep the contents (a later NonVolatile answers what
// was asked before, never Empty), which is what a caller that refills on Empty needs.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}
- (id<MTLHeap>)heap
{
    return (id<MTLHeap>)_heap;
}
- (void)makeAliasable
{}
- (BOOL)isAliasable
{
    return NO;
}
- (IOSurfaceRef)iosurface
{
    return nullptr;
}
@end

typedef NS_ENUM(uint8_t, RMTextureKind) {
    RMTexturePool,
    RMTextureSurface,
    RMTextureBuffer,
    RMTextureView,
    RMTextureMemoryless,
    RMTextureTwinOnly
};

@interface RMTexture : RMObject {
@public
    raw::Texture _raw;
    bool _rawOk;  // the raw path can draw with it (else every command buffer using it goes to m2v)
    RMTextureKind _kind;
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _surface;
    NSUInteger _plane;
    RMTexture *_parent;  // a view's
    RMBuffer *_buffer;   // a texture over a buffer's
    NSUInteger _offset, _rowBytes;
    std::atomic<bool> _gpuDirty;
    MTLPurgeableState _purgeable;
    uint8_t _pred;  // why the raw path does not keep it (RMPred; 0: it does)
}
@end

@implementation RMTexture
+ (Class)twinClass
{
    return NSClassFromString(@"NBTexture");
}
// SPI: the descriptor's (0 for every texture the raw path keeps, rawShape)
- (NSUInteger)rotation
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [_descriptor respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(_descriptor, rotation)
                                                     : 0;
}
- (void)dealloc
{
    _twin = nil;
    if (_rawOk)
        if (gRM) gRM->raw->destroyTexture(_raw);
    if (_surface) CFRelease(_surface);
}
- (id)rmTwin
{
    @synchronized(self) {
        if (_twin) return _twin;
        id<MTLDevice> m2v = gRM->fallback;
        switch (_kind) {
        case RMTextureSurface:
            _twin = [m2v newTextureWithDescriptor:_descriptor iosurface:_surface plane:_plane];
            break;
        case RMTexturePool: {  // m2v's in-place texture over the same pool pages (as an IOSurface's)
            const size_t page = size_t(getpagesize());
            id<MTLBuffer> pages = [m2v newBufferWithBytesNoCopy:_raw.span.cpu
                                                         length:(NSUInteger(_raw.span.bytes) + page - 1) & ~(page - 1)
                                                        options:MTLResourceStorageModeShared
                                                    deallocator:nil];
            if (pages && native_bridge_metal_texture_in_place)
                _twin = native_bridge_metal_texture_in_place(pages, _descriptor, 0, _raw.pitch);
            break;
        }
        case RMTextureBuffer:
            if (id<MTLBuffer> b = [_buffer rmTwin]) {
                _twin = native_bridge_metal_texture_in_place
                            ? native_bridge_metal_texture_in_place(b, _descriptor, _offset, _rowBytes)
                            : [b newTextureWithDescriptor:_descriptor offset:_offset bytesPerRow:_rowBytes];
            }
            break;
        case RMTextureView:
            _twin = [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat];
            break;
        case RMTextureMemoryless: _twin = [m2v newTextureWithDescriptor:_descriptor]; break;
        case RMTextureTwinOnly: break;
        }
        if (!_twin)
            once([NSString stringWithFormat:@"no m2v twin for a %ux%u texture of format %lu (kind %u)", _raw.width,
                                            _raw.height, (unsigned long)_descriptor.pixelFormat, unsigned(_kind)]);
        return _twin;
    }
}
- (MTLTextureType)textureType
{
    return _descriptor.textureType;
}
- (MTLPixelFormat)pixelFormat
{
    return _descriptor.pixelFormat;
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
    return _descriptor.mipmapLevelCount;
}
- (NSUInteger)arrayLength
{
    return _descriptor.arrayLength;
}
- (NSUInteger)sampleCount
{
    return _descriptor.sampleCount;
}
- (MTLTextureUsage)usage
{
    return _descriptor.usage;
}
- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}
- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}
- (BOOL)isFramebufferOnly
{
    return NO;
}
- (BOOL)allowGPUOptimizedContents
{
    return _descriptor.allowGPUOptimizedContents;
}
- (IOSurfaceRef)iosurface
{
    return _surface;
}
- (NSUInteger)iosurfacePlane
{
    return _plane;
}
- (id<MTLTexture>)parentTexture
{
    return (id<MTLTexture>)_parent;
}
- (id<MTLBuffer>)buffer
{
    return (id<MTLBuffer>)_buffer;
}
- (NSUInteger)bufferOffset
{
    return _offset;
}
- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}
- (NSUInteger)parentRelativeLevel
{
    return 0;
}
- (NSUInteger)parentRelativeSlice
{
    return 0;
}
- (BOOL)isShareable
{
    return NO;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}
- (void)makeAliasable
{}
- (BOOL)isAliasable
{
    return NO;
}
- (id<MTLHeap>)heap
{
    return nil;
}
// CPU writes into the texture's own pages (level 0, slice 0, a 2D texture), cleaned to memory at once: a later command buffer's
// GPU reads see them. Other textures are m2v's (their twin's replaceRegion).
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] replaceRegion:r
                                         mipmapLevel:level
                                               slice:slice
                                           withBytes:bytes
                                         bytesPerRow:row
                                       bytesPerImage:image];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height) return;
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *to = _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp;
        memcpy(to, (const uint8_t *)bytes + y * row, line);
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(to) & ~uintptr_t(63); a < uintptr_t(to) + line; a += 64)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}
// CPU reads of what the GPU wrote: its lines invalidated first (once per GPU write), then the rows
- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] getBytes:bytes
                                    bytesPerRow:row
                                  bytesPerImage:image
                                     fromRegion:r
                                    mipmapLevel:level
                                          slice:slice];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height) return;
#if defined(__aarch64__)
    if (_gpuDirty.exchange(false)) {
        for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63);
             a < uintptr_t(_raw.cpu) + uint64_t(_raw.pitch) * _raw.height; a += 64)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)bytes + y * row, _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp, line);
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
// A view: the same memory under another format's header (raw), or m2v's view of the twin
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_kind = RMTextureView;
    v->_parent = self;
    if (_rawOk && !_raw.memoryless && raw::formatInfo(uint32_t(format)))
        v->_rawOk = gRM->raw->createView(_raw, uint32_t(format), v->_raw);
    if (!v->_rawOk)
        refuseTexture(v, PredTexView, format, _rawOk, @"view",
                      [NSString stringWithFormat:@"parent kind %u pred %s", unsigned(_kind), kPredNames[_pred]]);
    if (!v->_rawOk && ![v rmTwin]) return nil;
    return (id<MTLTexture>)v;
}
@end

static void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra)
{
    t->_pred = p;
    if (!predLogged(p))
        logPred(p, a, b,
                [NSString
                    stringWithFormat:@"(%@ %@%@%@)", kind, describe(t->_descriptor), extra ? @" " : @"", extra ?: @""]);
}

@implementation RMBuffer (Textures)
// A texture over the buffer's bytes: the same memory under a texture header (raw), or m2v's in-place texture over the twin
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    RMTexture *t = [RMTexture new];
    t->_device = _device;
    t->_descriptor = [d copy];
    t->_kind = RMTextureBuffer;
    t->_buffer = self;
    t->_offset = offset;
    t->_rowBytes = row;
    if (rawShape(d) && d.storageMode != MTLStorageModeMemoryless)
        t->_rawOk = gRM->raw->createBufferTexture(_raw, offset, uint32_t(d.width), uint32_t(d.height), uint32_t(row),
                                                  uint32_t(d.pixelFormat), t->_raw);
    if (!t->_rawOk) {  // which term (createBufferTexture's, recomputed: not the hot path)
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0), at = (_raw.va + offset) & 31;
        NSString *extra = [NSString stringWithFormat:@"offset %lu bytesPerRow %lu buffer %llu", (unsigned long)offset,
                                                     (unsigned long)row, (unsigned long long)_raw.length];
        if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"buffer", extra);
        else if (at)
            refuseTexture(t, PredTexBufBase, at, offset, @"buffer", extra);
        else if ((row & 31) || row < packed)
            refuseTexture(t, PredTexBufPitch, row, packed, @"buffer", extra);
        else
            refuseTexture(t, PredTexBufOther, offset + uint64_t(row) * d.height, _raw.length, @"buffer", extra);
    }
    if (!t->_rawOk && ![t rmTwin]) return nil;
    return (id<MTLTexture>)t;
}
// SPI (Core Animation): the same, one 2D image
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    (void)image;
    return [self newTextureWithDescriptor:d offset:offset bytesPerRow:row];
}
@end

// Resource calls Core Animation makes on every texture and buffer that need no m2v twin (forwarding them would make one, an m2v
// import of memory the raw path draws from). Ownership is accounting only (the twin's, when it has one); no content is protected
// on this device; waitUntilComplete waits for the raw work submitted so far (the kernel's fence interrupt) and the twin's.
@interface RMObject (Resource)
@end
@implementation RMObject (Resource)
- (kern_return_t)setOwnerWithIdentity:(mach_port_t)identity
{
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd])
        return ((kern_return_t (*)(id, SEL, mach_port_t))objc_msgSend)(twin, _cmd, identity);
    return KERN_SUCCESS;
}
- (uint64_t)protectionOptions
{
    return 0;
}
- (void)waitUntilComplete
{
    if (gRM && gRM->raw) gRM->raw->wait(gRM->raw->submitted());
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd]) ((void (*)(id, SEL))objc_msgSend)(twin, _cmd);
}
@end

@interface RMSampler : RMObject {
@public
    raw::Sampler _raw;
    MTLSamplerDescriptor *_descriptor;
}
@end
@implementation RMSampler
+ (Class)twinClass
{
    return NSClassFromString(@"NBSampler");
}
- (void)dealloc
{
    if (gRM) gRM->raw->destroySampler(_raw);
}
- (id)rmTwin
{
    @synchronized(self) {
        if (!_twin) _twin = [gRM->fallback newSamplerStateWithDescriptor:_descriptor];
        return _twin;
    }
}
@end

@interface RMPipeline : RMObject {
@public
    raw::Pipeline *_raw;  // null: not in the pack (every command buffer using it goes to m2v)
    bool _constantBlend;
    bool _quartzCore;  // its functions are QuartzCore's (looked up in the pack)
}
@end
@implementation RMPipeline
+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderPipeline");
}
@end

@interface RMTwinObject : RMObject  // depth-stencil and compute states, and any m2v object the facade only carries
@end
@implementation RMTwinObject
@end

@implementation RMHeap
- (id)rmTwin
{
    return nil;  // the facade's heap is its own: its buffers and textures are ordinary facade resources
}
- (NSUInteger)size
{
    return _descriptor.size;
}
- (NSUInteger)usedSize
{
    return NSUInteger(_used.load());
}
- (NSUInteger)currentAllocatedSize
{
    return _descriptor.size;
}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    (void)alignment;
    const uint64_t used = _used.load();
    return used >= _descriptor.size ? 0 : NSUInteger(_descriptor.size - used);
}
- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}
- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}
- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}
- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}
- (MTLHeapType)type
{
    return _descriptor.type;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    (void)state;
    return MTLPurgeableStateNonVolatile;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if ([b isKindOfClass:[RMBuffer class]]) {
        ((RMBuffer *)b)->_heap = self;
        _used += length;
    }
    return (id<MTLBuffer>)b;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    (void)offset;
    return [self newBufferWithLength:length options:options];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [(id<MTLDevice>)_device newTextureWithDescriptor:d];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    (void)offset;
    return [self newTextureWithDescriptor:d];
}
@end

// ---- the tape ----
namespace {
enum Op : uint8_t {
    OpRender,
    OpBlit,
    OpCompute,
    OpEnd,
    OpPipeline,
    OpBytes,
    OpBuffer,
    OpBufferOffset,
    OpTexture,
    OpSampler,
    OpScissor,
    OpViewport,
    OpDraw,
    OpCull,
    OpWinding,
    OpDepthStencil,
    OpFillMode,
    OpBlendColor,
    OpStencilRef,
    OpDepthBias,
    OpBarrier,
    OpUse,
    OpUpdateFence,
    OpWaitFence,
    OpCopyTexture,
    OpCopyBuffer,
    OpCopyBufferToTexture,
    OpCopyTextureToBuffer,
    OpMipmaps,
    OpInvocation,
    OpSignalEvent,
    OpWaitEvent,
    OpDrawIndexed,
    OpVisibility,
};
// one record: `stage` 1 vertex, 2 fragment; `obj` unretained (the command buffer keeps it); `x` into the extras
struct Cmd {
    Op op;
    uint8_t stage;
    uint16_t index;
    uint32_t a, b;
    uint64_t c, x;
    void *obj, *obj2;
};
inline Cmd cmd(Op op, uint8_t stage = 0, uint16_t index = 0)
{
    Cmd c = {};
    c.op = op;
    c.stage = stage;
    c.index = index;
    return c;
}
}  // namespace

@interface RMQueue : RMObject {
@public
    dispatch_queue_t _completionQueue;
}
@end

@interface RMCommandBuffer : RMObject {
@public
    uint64_t _protection;  // setProtectionOptions: (kept, no effect)
    std::vector<Cmd> _tape;
    std::vector<uint8_t> _bytes;
    std::vector<uint64_t> _extra;
    std::vector<id> _owners;  // everything the tape names, retained until completion
    RMReason _reason;
    NSString *_why;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    MTLCommandBufferStatus _status;
    NSError *_error;
    RMQueue *_queue;
    bool _committed, _scheduledDone, _completedDone, _m2v;
    // scheduled() and finish() ran (their handlers taken): a handler added after goes at once, and scheduled() runs once (a
    // command buffer held for a shared event is scheduled at its commit, eventHeld)
    bool _scheduledSent, _completedSent;
    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _seqno;
    double _gpuStart, _gpuEnd;
    id _active;               // the encoder being recorded
    uint64_t _preds;          // every refusal predicate it hit (1 << RMPred)
    uint64_t _predA, _predB;  // the first one's values
    uint8_t _pred0;           // the first one
}
- (void)takeReason:(RMReason)reason why:(NSString *)why;
- (void)record:(const Cmd &)cmd;
- (void)retain:(id)object;
@end

// A refusal predicate hit while recording (or encoding) `cb`: into its mask; the first one's values kept; the process's first hit
// of `p` logged with them
static inline void hit(RMCommandBuffer *cb, RMPred p, uint64_t a = 0, uint64_t b = 0)
{
    cb->_preds |= 1ull << p;
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    if (!predLogged(p)) logPred(p, a, b, nil);
}

// ---- encoders: they record ----
@interface RMEncoder : RMObject {
@public
    RMCommandBuffer *_cb;
    bool _ended;
}
@end
@implementation RMEncoder
- (id)rmTwin
{
    return nil;
}
// What the m2v encoder of the same kind answers (respondsToSelector: as Core Animation's optional paths ask it today)
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector]) return YES;
    Class c = [[self class] twinClass];
    return c && [c instancesRespondToSelector:selector];
}
// A selector this encoder does not record itself (one the m2v encoder implements, or some MTL protocol declares): recorded as an
// invocation (objects mapped to twins at replay) and the command buffer goes to m2v.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector]) return s;
    Class c = [[self class] twinClass];
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil) return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}
- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}
- (void)forwardInvocation:(NSInvocation *)inv
{
    [inv retainArguments];
    [_cb retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    [_cb record:c];
    [_cb takeReason:WhyCall why:@"an encoder call recorded as an invocation"];
    hit(_cb, PredCallInvocation, uintptr_t(inv.selector));
    static std::mutex lock;  // each selector named once (a pointer set: no string unless it is new)
    static std::set<SEL> seen;
    bool fresh;
    {
        std::lock_guard<std::mutex> l(lock);
        fresh = seen.size() < 256 && seen.insert(inv.selector).second;
    }
    if (fresh)
        once([NSString
            stringWithFormat:@"invocation -[%s %s]", class_getName(object_getClass(self)), sel_getName(inv.selector)]);
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}
+ (Class)twinClass
{
    return nil;
}
- (void)endEncoding
{
    if (_ended) return;
    _ended = true;
    [_cb record:cmd(OpEnd)];
    _cb->_active = nil;
}
- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}
- (void)popDebugGroup
{}
- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}
- (void)updateFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpUpdateFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}
- (void)waitForFence:(id<MTLFence>)fence
{
    [_cb retain:fence];
    Cmd c = cmd(OpWaitFence);
    c.obj = (__bridge void *)fence;
    [_cb record:c];
}
@end

// A render encoder keeps what its draws need to decide, as they are recorded, whether the raw path draws them exactly as m2v
// would: the state, each bound buffer's address, the textures, the pass's formats. A draw it would not is the command buffer's
// reason then, so encodeRaw never meets it (a fallback costs no encoding).
@interface RMRenderEncoder : RMEncoder {
@public
    RMPipeline *_pipe;
    uint64_t _bufVa[3]
                   [31];  // stage 1 vertex, 2 fragment: the bound buffer's address plus offset (setBytes: 0, aligned)
    RMBuffer *_buf[3][31];
    RMTexture *_tex[16];   // fragment textures (unretained: the command buffer retains what it binds)
    uint32_t _formats[8];  // the pass's color attachments' formats as the raw path holds them (0: none)
    uint32_t _count;       // its attachment count (the last attachment's index + 1)
}
@end
@interface RMBlitEncoder : RMEncoder
@end
@interface RMComputeEncoder : RMEncoder
@end

static inline void setBinding(RMCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t c)
{
    if (object) [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = c;
    [cb record:m];
}

@implementation RMRenderEncoder
+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderEncoder");
}
// Core Animation's texture-and-sampler SPI (CA::OGL::MetalContext::draw, every textured draw), as the frontend answers it
- (void)setFragmentTexture:(id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    [self texture:2 object:t index:ti];
    [self sampler:2 object:s index:si];
}
// Core Animation sets the mode Disabled when its encoder state changes (update_encoder_state): nothing to do; any other mode
// counts occlusion, which the raw path does not
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    if (mode != MTLVisibilityResultModeDisabled) {
        [_cb takeReason:WhyCall why:@"a visibility result mode"];
        hit(_cb, PredCallVisibility, mode);
    }
    Cmd m = cmd(OpVisibility);
    m.a = uint32_t(mode);
    m.c = offset;
    [_cb record:m];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    if (![(id)buffer isKindOfClass:[RMBuffer class]] || instances != 1 || baseVertex || baseInstance) {
        [_cb takeReason:WhyCall why:@"an indexed draw of several instances or a base vertex"];
        hit(_cb, PredCallIndexedForm, instances, uint64_t(uint32_t(baseVertex)) | uint64_t(baseInstance) << 32);
    } else if ((indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) ||
               offset % (indexType == MTLIndexTypeUInt16 ? 2 : 4) ||
               offset + uint64_t(count) * (indexType == MTLIndexTypeUInt16 ? 2 : 4) >
                   ((RMBuffer *)buffer)->_raw.length) {
        [_cb takeReason:WhyEncode why:@"an indexed draw the raw path does not take"];
        hit(_cb, PredEncIndexedRange, offset, indexType == MTLIndexTypeUInt16 ? 2 : 4);
    }
    [self checkDraw:0];
    if (buffer) [_cb retain:buffer];
    Cmd m = cmd(OpDrawIndexed, uint8_t(type));
    m.a = uint32_t(count);
    m.b = uint32_t(indexType);
    m.c = offset;
    m.obj = (__bridge void *)buffer;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(instances);
    _cb->_extra.push_back(uint64_t(baseVertex));
    _cb->_extra.push_back(baseInstance);
    [_cb record:m];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
{
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:instances
                     baseVertex:0
                   baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
{
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:buffer
              indexBufferOffset:offset
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    RMPipeline *p = (RMPipeline *)state;
    if (![(id)p isKindOfClass:[RMPipeline class]]) {
        [_cb takeReason:WhyState why:@"a render state not made by this device"];
        hit(_cb, PredStateForeign);
    } else if (!p->_raw) {
        [_cb takeReason:WhyState why:@"a state not in the pack (its label: the line it was made with)"];
        hit(_cb, p->_quartzCore ? PredStateNotInPack : PredStateNotQuartzCore);
    } else if (p->_constantBlend) {
        [_cb takeReason:WhyState why:@"a state with constant blend factors"];
        hit(_cb, PredStateConstBlend);
    }
    _pipe = [(id)p isKindOfClass:[RMPipeline class]] ? p : nil;
    setBinding(_cb, OpPipeline, 0, 0, state, 0);
}
// What raw::RenderEncoder::prepare would refuse of a draw with the encoder's bindings, decided here (the same tests)
- (void)checkDraw:(NSUInteger)baseInstance
{
    if (!_pipe || !_pipe->_raw) return;  // no raw draw to decide (m2v drops a draw without a state too)
    const raw::Pipeline &pl = *_pipe->_raw;
    if (baseInstance) {
        [_cb takeReason:WhyEncode why:@"draw with a base instance"];
        return hit(_cb, PredEncBaseInstance, baseInstance);
    }
    for (uint32_t j = 0; j < 8; j++) {
        const uint32_t f = j < _count ? _formats[j] : 0;
        if (f != pl.colorFormats[j] && !(f && pl.colorFormats[j] == 0)) {
            [_cb takeReason:WhyEncode why:@"a state whose color formats differ from the pass's"];
            return hit(_cb, PredEncColorFormats, j, f);
        }
    }
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (e.kind == raw::EntryUbo) {
            // m2v's SlotBindOffset residual is not in the pack (every probe bound at offset 0); the device answers 256 for
            // minConstantBufferAlignmentBytes, so Core Animation's buffer pool binds there
            if (!e.samplerKind && e.stage < 3 && e.slot < 31 && (_bufVa[e.stage][e.slot] & 255)) {
                [_cb takeReason:WhyEncode why:@"a uniform buffer bound at an offset that is not a multiple of 256"];
                return hit(_cb, PredEncUboOffset, uint64_t(e.stage) << 8 | e.slot, _bufVa[e.stage][e.slot] & 255);
            }
        } else if (e.kind == raw::EntryTexture && e.samplerKind == raw::SamplerConstexpr && e.slot < 16 &&
                   _tex[e.slot] && _tex[e.slot]->_raw.addressMap) {
            [_cb takeReason:WhyEncode why:@"a planar texture sampled through a constexpr sampler"];
            return hit(_cb, PredEncPlanarClamp, e.slot, _tex[e.slot]->_raw.addressMap);
        }
    }
}
- (void)bytes:(int)stage data:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index
{
    if (stage >= 1 && stage <= 2 && index < 31) {
        _bufVa[stage][index] = 0;  // its own 256-byte-aligned upload chunk
        _buf[stage][index] = nil;
    }
    RMCommandBuffer *cb = _cb;
    Cmd m = cmd(OpBytes, uint8_t(stage), uint16_t(index));
    m.a = uint32_t(cb->_bytes.size());
    m.b = uint32_t(length);
    cb->_bytes.insert(cb->_bytes.end(), (const uint8_t *)bytes, (const uint8_t *)bytes + length);
    [cb record:m];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:1 data:bytes length:length index:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self bytes:2 data:bytes length:length index:index];
}
- (void)buffer:(int)stage object:(id<MTLBuffer>)b offset:(NSUInteger)offset index:(NSUInteger)index
{
    if (b && ![(id)b isKindOfClass:[RMBuffer class]]) {
        [_cb takeReason:WhyCall why:@"a buffer not made by this device"];
        hit(_cb, PredCallBufForeign, uint64_t(stage) << 8 | index, b.length);
    }
    if (stage >= 1 && stage <= 2 && index < 31) {
        RMBuffer *x = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
        _buf[stage][index] = x;
        _bufVa[stage][index] = x ? x->_raw.va + offset : 0;
    }
    setBinding(_cb, OpBuffer, stage, index, b, offset);
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:1 object:b offset:offset index:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self buffer:2 object:b offset:offset index:index];
}
- (void)bufferOffset:(int)stage offset:(NSUInteger)offset index:(NSUInteger)index
{
    if (index < 31 && _buf[stage][index]) _bufVa[stage][index] = _buf[stage][index]->_raw.va + offset;
    setBinding(_cb, OpBufferOffset, stage, index, nil, offset);
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:1 offset:offset index:index];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self bufferOffset:2 offset:offset index:index];
}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self buffer:1 object:b[k] offset:o[k] index:r.location + k];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self buffer:2 object:b[k] offset:o[k] index:r.location + k];
}
- (void)texture:(int)stage object:(id<MTLTexture>)t index:(NSUInteger)index
{
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (t && (!x || !x->_rawOk || !x->_raw.tic)) {
        [_cb takeReason:WhyTexture why:@"a texture the raw path does not keep"];
        if (!x) {
            if (!predLogged(PredTexForeign))
                logPred(PredTexForeign, t.pixelFormat, 0,
                        [NSString stringWithFormat:@"class %s", class_getName(object_getClass(t))]);
            hit(_cb, PredTexForeign, t.pixelFormat);
        } else
            hit(_cb, RMPred(x->_pred ?: (x->_rawOk ? PredEncNoTic : PredTexShape)), x->_descriptor.pixelFormat,
                x->_kind);
    }
    if (stage == 2 && index < 16) _tex[index] = x;
    setBinding(_cb, OpTexture, stage, index, t, 0);
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:2 object:t index:index];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index
{
    [self texture:1 object:t index:index];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self texture:2 object:t[k] index:r.location + k];
}
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self texture:1 object:t[k] index:r.location + k];
}
- (void)sampler:(int)stage object:(id<MTLSamplerState>)s index:(NSUInteger)index
{
    if (s && ![(id)s isKindOfClass:[RMSampler class]]) {
        [_cb takeReason:WhyCall why:@"a sampler not made by this device"];
        hit(_cb, PredCallSamplerForeign, index);
    }
    if (stage == 1 && s) {
        [_cb takeReason:WhyCall why:@"a vertex sampler"];
        hit(_cb, PredCallVertexSampler, index);
    }
    setBinding(_cb, OpSampler, stage, index, s, 0);
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:2 object:s index:index];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index
{
    [self sampler:1 object:s index:index];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++) [self sampler:2 object:s[k] index:r.location + k];
}
- (void)setScissorRect:(MTLScissorRect)r
{
    Cmd m = cmd(OpScissor);
    m.a = uint32_t(r.x);
    m.b = uint32_t(r.y);
    m.c = uint64_t(r.width) << 32 | uint32_t(r.height);
    [_cb record:m];
}
- (void)setViewport:(MTLViewport)v
{
    Cmd m = cmd(OpViewport);
    m.x = _cb->_extra.size();
    const double d[6] = {v.originX, v.originY, v.width, v.height, v.znear, v.zfar};
    for (double x : d) {
        uint64_t u;
        memcpy(&u, &x, 8);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}
- (void)setCullMode:(MTLCullMode)mode
{
    if (mode != MTLCullModeNone) {
        [_cb takeReason:WhyCall why:@"culling"];
        hit(_cb, PredCallCull, mode);
    }
    Cmd m = cmd(OpCull);
    m.a = uint32_t(mode);
    [_cb record:m];
}
- (void)setFrontFacingWinding:(MTLWinding)w
{
    Cmd m = cmd(OpWinding);
    m.a = uint32_t(w);
    [_cb record:m];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
    setBinding(_cb, OpDepthStencil, 0, 0, s,
               0);  // no Core Animation pass has a depth or stencil attachment (checked at the pass)
}
// SPI: lines one pixel wide are what the raw path draws (its default); any other width goes to m2v with the call
- (void)setLineWidth:(float)width
{
    if (width == 1.0f) return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[self methodSignatureForSelector:_cmd]];
    inv.selector = _cmd;
    [inv setArgument:&width atIndex:2];
    [self forwardInvocation:inv];
}
// No tile shaders (as m2v: the device says so); their calls stay m2v's, which refuses them
- (NSUInteger)tileWidth
{
    return 0;
}
- (NSUInteger)tileHeight
{
    return 0;
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)mode
{
    if (mode != MTLTriangleFillModeFill) {
        [_cb takeReason:WhyCall why:@"line fill"];
        hit(_cb, PredCallFillMode, mode);
    }
    Cmd m = cmd(OpFillMode);
    m.a = uint32_t(mode);
    [_cb record:m];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
    Cmd m = cmd(OpBlendColor);
    m.x = _cb->_extra.size();
    const float f[4] = {r, g, b, a};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}
- (void)setStencilReferenceValue:(uint32_t)value
{
    Cmd m = cmd(OpStencilRef);
    m.a = value;
    m.b = value;
    [_cb record:m];
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
    Cmd m = cmd(OpStencilRef);
    m.a = front;
    m.b = back;
    [_cb record:m];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp
{
    Cmd m = cmd(OpDepthBias);
    m.x = _cb->_extra.size();
    const float f[3] = {bias, slope, clamp};
    for (float v : f) {
        uint32_t u;
        memcpy(&u, &v, 4);
        _cb->_extra.push_back(u);
    }
    [_cb record:m];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)base
{
    [self checkDraw:base];
    Cmd m = cmd(OpDraw, uint8_t(type));
    m.a = uint32_t(start);
    m.b = uint32_t(count);
    m.c = uint64_t(instances) << 32 | uint32_t(base);
    [_cb record:m];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
                   afterStages:(MTLRenderStages)after
                  beforeStages:(MTLRenderStages)before
{
    Cmd m = cmd(OpBarrier);
    m.a = uint32_t(scope);
    m.b = uint32_t(after);
    m.c = uint64_t(before);
    [_cb record:m];
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{
    if (!resource) return;
    [_cb retain:resource];
    Cmd m = cmd(OpUse);
    m.obj = (__bridge void *)resource;
    m.a = uint32_t(usage);
    m.b = uint32_t(stages);
    [_cb record:m];
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    [self useResource:resource usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment];
}
- (void)useHeap:(id<MTLHeap>)heap
{
    (void)heap;  // the facade's heaps hold ordinary resources: nothing to make resident
}
- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages
{
    (void)heap;
    (void)stages;
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    (void)stages;
    [self updateFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    (void)stages;
    [self waitForFence:fence];
}
@end

@implementation RMBlitEncoder
+ (Class)twinClass
{
    return NSClassFromString(@"NBBlitEncoder");
}
- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    RMTexture *a = (RMTexture *)s, *b = (RMTexture *)d;
    const bool ok = [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMTexture class]] && a->_rawOk &&
                    b->_rawOk && !ss && !sl && !ds && !dl && size.depth == 1 && a->_raw.bpp == b->_raw.bpp &&
                    !a->_raw.memoryless && !b->_raw.memoryless && so.x + size.width <= a->_raw.width &&
                    so.y + size.height <= a->_raw.height && dorg.x + size.width <= b->_raw.width &&
                    dorg.y + size.height <= b->_raw.height;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture copy the raw path does not do (levels, slices, formats)"];
        hit(_cb, PredBlitTexCopy, ss | sl << 8 | ds << 16 | dl << 24,
            uint64_t(size.width) << 32 | uint32_t(size.height));
    } else if (a->_raw.addressMap ||
               b->_raw.addressMap) {  // Metal's copies move a planar texture's planes' bytes: m2v's
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, a->_raw.addressMap, b->_raw.addressMap);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[11] = {ss,
                            sl,
                            so.x,
                            so.y,
                            so.z,
                            size.width,
                            size.height,
                            size.depth,
                            ds,
                            dl,
                            uint64_t(dorg.x) | uint64_t(dorg.y) << 32};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 11);
    _cb->_extra.push_back(dorg.z);
    [_cb record:m];
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    [self copyFromTexture:s
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(s.width, s.height, 1)
                toTexture:d
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (s.mipmapLevelCount > 1 || s.arrayLength > 1 || s.textureType != MTLTextureType2D) {
        [_cb takeReason:WhyTexture why:@"a whole-texture copy of several levels or slices"];
        hit(_cb, PredBlitWholeTexCopy, s.mipmapLevelCount, s.arrayLength);
    }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dof
                  size:(NSUInteger)n
{
    RMBuffer *a = (RMBuffer *)s, *b = (RMBuffer *)d;
    if (![(id)a isKindOfClass:[RMBuffer class]] || ![(id)b isKindOfClass:[RMBuffer class]] || so + n > a->_raw.length ||
        dof + n > b->_raw.length) {
        [_cb takeReason:WhyCall why:@"a buffer copy out of range"];
        hit(_cb, PredCallBufCopyRange, so + n, dof + n);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = 0;
    m.c = so;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(dof);
    _cb->_extra.push_back(n);
    [_cb record:m];
}
- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    RMBuffer *a = (RMBuffer *)s;
    RMTexture *b = (RMTexture *)d;
    const bool ok = [(id)a isKindOfClass:[RMBuffer class]] && [(id)b isKindOfClass:[RMTexture class]] && b->_rawOk &&
                    !ds && !dl && size.depth == 1 && !b->_raw.memoryless &&
                    so + row * (size.height - 1) + size.width * b->_raw.bpp <= a->_raw.length &&
                    dorg.x + size.width <= b->_raw.width && dorg.y + size.height <= b->_raw.height;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a buffer-to-texture copy the raw path does not do"];
        hit(_cb, PredBlitBufToTex, row, uint64_t(ds) | uint64_t(dl) << 8 | uint64_t(size.depth) << 16);
    } else if (b->_raw.addressMap) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, 0, b->_raw.addressMap);
    } else if (row % b->_raw.bpp &&
               size.height > 1) {  // rows of whole texels (Metal's rule; the copy engine takes any)
        [_cb takeReason:WhyTexture why:@"a buffer-to-texture copy whose rows are not whole texels"];
        hit(_cb, PredEncBufToTexRow, row, b->_raw.bpp);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyBufferToTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        so, row, image, size.width, size.height, size.depth, ds, dl, uint64_t(dorg.x) | uint64_t(dorg.y) << 32, dorg.z};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
    [_cb record:m];
}
- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
                options:(MTLBlitOption)options
{
    if (options) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromBuffer:s
               sourceOffset:so
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:d
           destinationSlice:ds
           destinationLevel:dl
          destinationOrigin:dorg];
}
- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    RMTexture *a = (RMTexture *)s;
    RMBuffer *b = (RMBuffer *)d;
    const bool ok = [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMBuffer class]] && a->_rawOk &&
                    !ss && !sl && size.depth == 1 && !a->_raw.memoryless &&
                    dof + row * (size.height - 1) + size.width * a->_raw.bpp <= b->_raw.length &&
                    so.x + size.width <= a->_raw.width && so.y + size.height <= a->_raw.height;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture-to-buffer copy the raw path does not do"];
        hit(_cb, PredBlitTexToBuf, row, uint64_t(ss) | uint64_t(sl) << 8 | uint64_t(size.depth) << 16);
    } else if (a->_raw.addressMap) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, a->_raw.addressMap, 0);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyTextureToBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        ss, sl, uint64_t(so.x) | uint64_t(so.y) << 32, so.z, size.width, size.height, size.depth, dof, row, image};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
    [_cb record:m];
}
- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
                     options:(MTLBlitOption)options
{
    if (options) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromTexture:s
                     sourceSlice:ss
                     sourceLevel:sl
                    sourceOrigin:so
                      sourceSize:size
                        toBuffer:d
               destinationOffset:dof
          destinationBytesPerRow:row
        destinationBytesPerImage:image];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    [_cb takeReason:WhyMipmaps why:@"generateMipmapsForTexture"];
    hit(_cb, PredMipmaps, t.pixelFormat, t.mipmapLevelCount);
    setBinding(_cb, OpMipmaps, 0, 0, t, 0);
}
- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;  // shared memory: nothing to make visible
}
- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l
{
    (void)t;
    (void)s;
    (void)l;
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t
{
    (void)t;  // pitch-linear memory either way
}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t
{
    (void)t;
}
@end

@implementation RMComputeEncoder  // everything recorded as invocations (the command buffer is m2v's): see RMEncoder
+ (Class)twinClass
{
    return NSClassFromString(@"NBComputeEncoder");
}
@end

// ---- the device's submission machinery ----
namespace {}  // namespace

static void markWritten(RMCommandBuffer *cb);
static void finish(RMCommandBuffer *cb, NSError *error);
static bool heldBehindEvent();

@implementation RMCommandBuffer
- (void)retain:(id)object
{
    if (!object || (!_owners.empty() && _owners.back() == object)) return;
    _owners.push_back(object);
}
- (void)record:(const Cmd &)cmd
{
    _tape.push_back(cmd);
}
- (void)takeReason:(RMReason)reason why:(NSString *)why
{
    if (_reason != WhyRaw) return;
    _reason = reason;
    _why = why;
}
- (id)rmTwin
{
    return nil;
}
- (MTLCommandBufferStatus)status
{
    std::lock_guard<std::mutex> l(_lock);
    return _status;
}
- (NSError *)error
{
    return _error;
}
- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}
- (BOOL)retainedReferences
{
    return YES;
}
- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}
- (id<MTLLogContainer>)logs
{
    return nil;
}
- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}
- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}
- (CFTimeInterval)kernelStartTime
{
    return _gpuStart;
}
- (CFTimeInterval)kernelEndTime
{
    return _gpuEnd;
}
- (void)begin:(RMEncoder *)e
{
    if (_active) [(RMEncoder *)_active endEncoding];  // Metal: one encoder at a time
    e->_device = _device;
    e->_cb = self;
    _active = e;
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    RMRenderEncoder *e = [RMRenderEncoder new];
    [self begin:e];
    MTLRenderPassDescriptor *pass = [d copy];
    [self retain:pass];
    for (unsigned c = 0; c < 8; c++) {
        id<MTLTexture> t = pass.colorAttachments[c].texture;
        if (!t) continue;
        [self retain:t];
        [self retain:pass.colorAttachments[c].resolveTexture];
        RMTexture *x = (RMTexture *)t;
        if (![(id)x isKindOfClass:[RMTexture class]] || !x->_rawOk || !x->_raw.renderable) {
            [self takeReason:WhyPass why:@"a color attachment the raw path does not render to"];
            const bool own = [(id)x isKindOfClass:[RMTexture class]];
            // a = format; b = attachment | kind << 8 | the texture's own predicate << 16 (0: kept, not renderable)
            const uint64_t b = c | (own ? uint64_t(x->_kind) : 0xff) << 8 | (own ? uint64_t(x->_pred) : 0) << 16;
            if (own && x->_rawOk &&
                !predLogged(PredPassAttFormat))  // kept, not a target: which term (raw_gpu.cpp linearTarget)
                logPred(
                    PredPassAttFormat, t.pixelFormat, b,
                    [NSString stringWithFormat:@"(kept, not a target: %ux%u pitch %u va&127 %llu, a pitch target needs "
                                               @"both multiples of 128)",
                                               x->_raw.width, x->_raw.height, x->_raw.pitch,
                                               (unsigned long long)(x->_raw.va & 127)]);
            hit(self, PredPassAttFormat, t.pixelFormat, b);
        } else if (x->_raw.planar || x->_raw.ycbcr) {
            [self takeReason:WhyPass why:@"a planar texture as a render target"];
            hit(self, PredEncPlanarRT, t.pixelFormat, c);
        } else if (pass.colorAttachments[c].level || pass.colorAttachments[c].slice ||
                   pass.colorAttachments[c].depthPlane || pass.colorAttachments[c].resolveTexture) {
            [self takeReason:WhyPass why:@"a color attachment level, slice or resolve"];
            hit(self, PredPassAttLevel, pass.colorAttachments[c].level | pass.colorAttachments[c].slice << 8,
                pass.colorAttachments[c].resolveTexture != nil);
        } else {
            e->_formats[c] = x->_raw.format;
            e->_count = c + 1;
        }
    }
    // renderTargetWidth / renderTargetHeight: the render area's bounds (raw::PassDesc width, height)
    if (pass.depthAttachment.texture || pass.stencilAttachment.texture || pass.visibilityResultBuffer ||
        (pass.renderTargetArrayLength > 1) || pass.rasterizationRateMap) {
        [self retain:pass.depthAttachment.texture];
        [self retain:pass.stencilAttachment.texture];
        [self retain:pass.visibilityResultBuffer];
        [self takeReason:WhyPass why:@"a depth, stencil or visibility attachment"];
        if (pass.depthAttachment.texture || pass.stencilAttachment.texture)
            hit(self, PredPassDepthStencil, pass.depthAttachment.texture.pixelFormat,
                pass.stencilAttachment.texture.pixelFormat);
        else
            hit(self, PredPassOther, pass.visibilityResultBuffer != nil,
                pass.renderTargetArrayLength | uint64_t(pass.rasterizationRateMap != nil) << 32);
    }
    Cmd m = cmd(OpRender);
    m.obj = (__bridge void *)pass;
    [self record:m];
    return (id<MTLRenderCommandEncoder>)e;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    RMBlitEncoder *e = [RMBlitEncoder new];
    [self begin:e];
    [self record:cmd(OpBlit)];
    return (id<MTLBlitCommandEncoder>)e;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d
{
    (void)d;
    return [self blitCommandEncoder];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    RMComputeEncoder *e = [RMComputeEncoder new];
    [self begin:e];
    Cmd m = cmd(OpCompute);
    m.a = uint32_t(type);
    [self record:m];
    [self takeReason:WhyCompute why:@"a compute encoder"];
    hit(self, PredCompute, type);
    return (id<MTLComputeCommandEncoder>)e;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    return [self computeCommandEncoderWithDispatchType:d.dispatchType];
}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal
{
    // the m2v device's shared events: the raw path signals them from the CPU once its work completes, and waits for them on the
    // CPU before its work is submitted; any other event is m2v's alone
    if (![(id)event conformsToProtocol:@protocol(MTLSharedEvent)] ||
        ![(id)event respondsToSelector:@selector(setSignaledValue:)]) {
        [self takeReason:WhyEvent why:@"an event that is not a shared event"];
        hit(self, PredEvent, signal);
    }
    [self retain:event];
    Cmd m = cmd(signal ? OpSignalEvent : OpWaitEvent);
    m.obj = (__bridge void *)event;
    m.c = value;
    [self record:m];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false];
}
- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}
// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}
- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}
- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}
- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}
- (void)popDebugGroup
{}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block) return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        now = _scheduledSent;
        if (!now) [_scheduledHandlers addObject:[block copy]];
    }
    if (now) dispatch_async(gRM->handlers, ^{ block((id<MTLCommandBuffer>)self); });
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block) return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now) [_completedHandlers addObject:[block copy]];
    }
    if (now) block((id<MTLCommandBuffer>)self);
}
- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) { [drawable present]; }];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) { [drawable presentAtTime:t]; }];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) { [drawable presentAfterMinimumDuration:d]; }];
}
- (void)enqueue
{}
- (void)commit
{
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_committed) return;
        _committed = true;
        _status = MTLCommandBufferStatusCommitted;
    }
    if (_active) [(RMEncoder *)_active endEncoding];
    [(RMDevice *)_device rmCommit:self];
}
- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}
- (void)waitUntilScheduled
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed) return;
    if (_changed.wait_for(l, kHeldGrace, [&] { return _scheduledDone; })) return;
    // Behind a shared event no one reached yet (its own wait, or an earlier command buffer's that holds it in order): Metal's
    // command buffer is scheduled there, its GPU waiting for the event, so this returns. Core Animation's render thread flushes
    // with waitUntilScheduled every frame, and forces the per-surface event of a client that died or was suspended after 5 s
    // (CA::WindowServer::SharedEvent::is_complete_with_timeout): blocked here it never could (r7n: AegirPoster died, r7o:
    // PosterKit's scenes were suspended; the display froze for good).
    l.unlock();
    if (heldBehindEvent()) {
        gRM->stats.heldReturns++;
        once(@"waitUntilScheduled of a command buffer behind a shared event not reached yet: returns after 500 ms, as "
             @"Metal's "
             @"(its GPU waits for the event)");
        return;
    }
    l.lock();
    if (!_changed.wait_for(l, kWaitLimit, [&] { return _scheduledDone; }))
        waitTooLong("waitUntilScheduled of command buffer", (__bridge void *)self);
}
- (void)waitUntilCompleted
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed) return;
    if (!_changed.wait_for(l, kWaitLimit, [&] { return _completedDone; }))
        waitTooLong("waitUntilCompleted of command buffer", (__bridge void *)self);
}
// A selector a command buffer does not answer here: recorded for m2v (the command buffer goes there), answered zero.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector]) return s;
    Class c = NSClassFromString(@"NBCommandBuffer");
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil) return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}
- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector]) return YES;
    Class c = NSClassFromString(@"NBCommandBuffer");
    return c && [c instancesRespondToSelector:selector];
}
- (void)forwardInvocation:(NSInvocation *)inv
{
    [inv retainArguments];
    [self retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    c.stage = 0xff;  // the command buffer's own, not an encoder's
    [self record:c];
    [self takeReason:WhyCall why:@"a command buffer call recorded as an invocation"];
    hit(self, PredCallCBInvocation, uintptr_t(inv.selector));
    static std::mutex lock;  // each selector named once
    static std::set<SEL> seen;
    bool fresh;
    {
        std::lock_guard<std::mutex> l(lock);
        fresh = seen.size() < 256 && seen.insert(inv.selector).second;
    }
    if (fresh) once([NSString stringWithFormat:@"invocation -[MTLCommandBuffer %s]", sel_getName(inv.selector)]);
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}
@end

@implementation RMQueue
+ (Class)twinClass
{
    return NSClassFromString(@"NBQueue");
}
- (id)rmTwin
{
    @synchronized(self) {
        if (!_twin) _twin = [gRM->fallback newCommandQueue];
        return _twin;
    }
}
- (id<MTLCommandBuffer>)commandBuffer
{
    RMCommandBuffer *b = [RMCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_scheduledHandlers = [NSMutableArray array];
    b->_completedHandlers = [NSMutableArray array];
    b->_status = MTLCommandBufferStatusNotEnqueued;
    b->_tape.reserve(512);
    b->_owners.reserve(256);
    b->_bytes.reserve(16384);
    return (id<MTLCommandBuffer>)b;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (raw_gpu.h)
}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}
- (void)insertDebugCaptureBoundary
{}
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}
- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}
@end

// ---- object mapping for the m2v replay ----
static inline id twinOf(id object)
{
    if (!object) return nil;
    if ([object isKindOfClass:[RMObject class]]) return [(RMObject *)object rmTwin];
    return object;  // an m2v object already (events, fences, anything the device forwarded)
}

static MTLRenderPassDescriptor *twinPass(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDescriptor *d = [pass copy];
    for (unsigned c = 0; c < 8; c++) {
        d.colorAttachments[c].texture = twinOf(pass.colorAttachments[c].texture);
        d.colorAttachments[c].resolveTexture = twinOf(pass.colorAttachments[c].resolveTexture);
    }
    d.depthAttachment.texture = twinOf(pass.depthAttachment.texture);
    d.depthAttachment.resolveTexture = twinOf(pass.depthAttachment.resolveTexture);
    d.stencilAttachment.texture = twinOf(pass.stencilAttachment.texture);
    d.stencilAttachment.resolveTexture = twinOf(pass.stencilAttachment.resolveTexture);
    d.visibilityResultBuffer = twinOf(pass.visibilityResultBuffer);
    return d;
}

// An invocation recorded on a facade encoder or command buffer, sent to the m2v one: object arguments become their twins.
static void replayInvocation(NSInvocation *inv, id target)
{
    NSMethodSignature *sig = inv.methodSignature;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = [sig getArgumentTypeAtIndex:i];
        while (*t == 'r' || *t == 'n' || *t == 'N' || *t == 'o' || *t == 'O' || *t == 'R' || *t == 'V') t++;
        if (*t == '@') {
            __unsafe_unretained id arg = nil;
            [inv getArgument:&arg atIndex:i];
            id mapped = twinOf(arg);
            if (mapped != arg) {
                __unsafe_unretained id m = mapped;
                [inv setArgument:&m atIndex:i];
            }
        } else if (t[0] == '^' && t[1] == '@') {
            // an array of objects (set...s:withRange:, useResources:count:): mapped in place over a copy the invocation keeps
            id __unsafe_unretained *array = nullptr;
            [inv getArgument:&array atIndex:i];
            NSUInteger count = 0;
            for (NSUInteger k = i + 1; k < sig.numberOfArguments && !count; k++) {
                const char *u = [sig getArgumentTypeAtIndex:k];
                if (u[0] == '{') {  // NSRange
                    NSRange r = {};
                    [inv getArgument:&r atIndex:k];
                    count = r.location + r.length;
                } else if (u[0] == 'Q' || u[0] == 'L' || u[0] == 'I') {
                    [inv getArgument:&count atIndex:k];
                }
            }
            if (array && count) {
                NSMutableData *copy = [NSMutableData dataWithLength:count * sizeof(id)];
                id __unsafe_unretained *out = (id __unsafe_unretained *)copy.mutableBytes;
                NSMutableArray *keep = [NSMutableArray array];
                for (NSUInteger k = 0; k < count; k++) {
                    id m = twinOf(array[k]);
                    if (m) [keep addObject:m];
                    out[k] = m;
                }
                objc_setAssociatedObject(inv, (__bridge const void *)copy, @[ copy, keep ], OBJC_ASSOCIATION_RETAIN);
                id __unsafe_unretained *p = out;
                [inv setArgument:&p atIndex:i];
            }
        }
    }
    [inv invokeWithTarget:target];
}

static uint64_t extra(RMCommandBuffer *cb, uint64_t i)
{
    return cb->_extra[i];
}
static double extraDouble(RMCommandBuffer *cb, uint64_t i)
{
    double d;
    memcpy(&d, &cb->_extra[i], 8);
    return d;
}
static float extraFloat(RMCommandBuffer *cb, uint64_t i)
{
    const uint32_t u = uint32_t(cb->_extra[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// The tape into an m2v command buffer, every object its twin; the CPU lines of the buffers it names cleaned first.
static id<MTLCommandBuffer> replay(RMCommandBuffer *cb)
{
    id<MTLCommandQueue> queue = [cb->_queue rmTwin];
    id<MTLCommandBuffer> m = [queue commandBuffer];
    if (!m) return nil;
    m.label = cb.label;
    id enc = nil;
    // What the CPU wrote that this work reads, cleaned to memory before m2v's submission (m2v cleans an import only when it
    // makes it): the ranges a draw of a pack state reads, as raw::RenderEncoder::prepare cleans them (its uniform windows, its
    // vertices, its indices), the ranges a copy reads (and writes: cleaned and invalidated, so no dirty line lands over the
    // GPU's bytes); a whole buffer only where the tape does not say (a state not in the pack, a compute or other recorded call,
    // a resource it uses)
    std::set<RMBuffer *> buffers;  // whole
    struct Range {
        const uint8_t *at;
        uint64_t bytes;
        bool flush;
    };
    std::vector<Range> ranges;
    auto range = [&](RMBuffer *b, uint64_t offset, uint64_t bytes, bool flush = false) {
        if (!b || offset >= b->_raw.length) return;
        ranges.push_back({b->_raw.cpu + offset, std::min(bytes, b->_raw.length - offset), flush});
    };
    auto use = [&](void *o) -> id {
        id x = (__bridge id)o;
        if ([x isKindOfClass:[RMBuffer class]]) buffers.insert((RMBuffer *)x);
        return twinOf(x);
    };
    // a render encoder's bindings, for its draws' ranges
    RMBuffer *bound[3][31] = {};
    uint64_t boundOffset[3][31] = {};
    RMPipeline *pipe = nil;
    auto drawn = [&](uint32_t first, uint32_t count) {  // the ranges a draw of `count` vertices from `first` reads
        if (!pipe || !pipe->_raw) {                     // not the pack's: every buffer bound, whole
            for (auto &stage : bound)
                for (RMBuffer *b : stage)
                    if (b) buffers.insert(b);
            return;
        }
        const raw::PackPipeline &r = *pipe->_raw->rec;
        for (uint32_t i = 0; i < r.entries; i++) {
            const raw::PackEntry &e = r.entry[i];
            if (e.kind != raw::EntryUbo || e.stage > 2 || e.slot > 30) continue;
            RMBuffer *b = bound[e.stage][e.slot];
            if (!b) continue;
            const uint64_t at = boundOffset[e.stage][e.slot], size = b->_raw.length - std::min(at, b->_raw.length);
            const uint64_t window = (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16;
            uint64_t bytes = std::min<uint64_t>((size + 15) & ~uint64_t(15), 0x10000);
            if (window && window < bytes) bytes = window;
            range(b, at, bytes);
        }
        if (r.vertexSlot <= 30 && r.vertexStride)
            if (RMBuffer *b = bound[1][r.vertexSlot])
                range(b, boundOffset[1][r.vertexSlot] + uint64_t(first) * r.vertexStride,
                      uint64_t(count) * r.vertexStride);
    };
    for (const Cmd &c : cb->_tape) {
        switch (c.op) {
        case OpRender:
            enc = [m renderCommandEncoderWithDescriptor:twinPass((__bridge MTLRenderPassDescriptor *)c.obj)];
            memset(bound, 0, sizeof bound);
            pipe = nil;
            break;
        case OpBlit: enc = [m blitCommandEncoder]; break;
        case OpCompute: enc = [m computeCommandEncoderWithDispatchType:MTLDispatchType(c.a)]; break;
        case OpEnd:
            [enc endEncoding];
            enc = nil;
            break;
        case OpPipeline:
            pipe = [(__bridge id)c.obj isKindOfClass:[RMPipeline class]] ? (__bridge RMPipeline *)c.obj : nil;
            [enc setRenderPipelineState:twinOf((__bridge id)c.obj)];
            break;
        case OpBytes:
            if (c.stage <= 2 && c.index <= 30) bound[c.stage][c.index] = nil;
            if (c.stage == 1)
                [enc setVertexBytes:cb->_bytes.data() + c.a length:c.b atIndex:c.index];
            else
                [enc setFragmentBytes:cb->_bytes.data() + c.a length:c.b atIndex:c.index];
            break;
        case OpBuffer: {
            id x = (__bridge id)c.obj;
            if (c.stage <= 2 && c.index <= 30) {
                bound[c.stage][c.index] = [x isKindOfClass:[RMBuffer class]] ? (RMBuffer *)x : nil;
                boundOffset[c.stage][c.index] = c.c;
            }
            if (c.stage == 1)
                [enc setVertexBuffer:twinOf(x) offset:c.c atIndex:c.index];
            else
                [enc setFragmentBuffer:twinOf(x) offset:c.c atIndex:c.index];
            break;
        }
        case OpBufferOffset:
            if (c.stage <= 2 && c.index <= 30) boundOffset[c.stage][c.index] = c.c;
            if (c.stage == 1)
                [enc setVertexBufferOffset:c.c atIndex:c.index];
            else
                [enc setFragmentBufferOffset:c.c atIndex:c.index];
            break;
        case OpTexture:
            if (c.stage == 1)
                [enc setVertexTexture:twinOf((__bridge id)c.obj) atIndex:c.index];
            else
                [enc setFragmentTexture:twinOf((__bridge id)c.obj) atIndex:c.index];
            break;
        case OpSampler:
            if (c.stage == 1)
                [enc setVertexSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            else
                [enc setFragmentSamplerState:twinOf((__bridge id)c.obj) atIndex:c.index];
            break;
        case OpScissor:
            [enc setScissorRect:(MTLScissorRect){c.a, c.b, NSUInteger(c.c >> 32), NSUInteger(uint32_t(c.c))}];
            break;
        case OpViewport:
            [enc setViewport:(MTLViewport){extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                                           extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4),
                                           extraDouble(cb, c.x + 5)}];
            break;
        case OpDraw:
            drawn(c.a, c.b);
            [enc drawPrimitives:MTLPrimitiveType(c.stage)
                    vertexStart:c.a
                    vertexCount:c.b
                  instanceCount:NSUInteger(c.c >> 32)
                   baseInstance:NSUInteger(uint32_t(c.c))];
            break;
        case OpCull: [enc setCullMode:MTLCullMode(c.a)]; break;
        case OpWinding: [enc setFrontFacingWinding:MTLWinding(c.a)]; break;
        case OpDepthStencil: [enc setDepthStencilState:twinOf((__bridge id)c.obj)]; break;
        case OpFillMode: [enc setTriangleFillMode:MTLTriangleFillMode(c.a)]; break;
        case OpBlendColor:
            [enc setBlendColorRed:extraFloat(cb, c.x)
                            green:extraFloat(cb, c.x + 1)
                             blue:extraFloat(cb, c.x + 2)
                            alpha:extraFloat(cb, c.x + 3)];
            break;
        case OpStencilRef: [enc setStencilFrontReferenceValue:c.a backReferenceValue:c.b]; break;
        case OpDepthBias:
            [enc setDepthBias:extraFloat(cb, c.x) slopeScale:extraFloat(cb, c.x + 1) clamp:extraFloat(cb, c.x + 2)];
            break;
        case OpBarrier:
            [enc memoryBarrierWithScope:MTLBarrierScope(c.a)
                            afterStages:MTLRenderStages(c.b)
                           beforeStages:MTLRenderStages(c.c)];
            break;
        case OpUse: [enc useResource:use(c.obj) usage:MTLResourceUsage(c.a) stages:MTLRenderStages(c.b)]; break;
        case OpUpdateFence: [enc updateFence:(__bridge id<MTLFence>)c.obj]; break;
        case OpWaitFence: [enc waitForFence:(__bridge id<MTLFence>)c.obj]; break;
        case OpCopyTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 10);
            [enc copyFromTexture:twinOf((__bridge id)c.obj)
                      sourceSlice:extra(cb, x)
                      sourceLevel:extra(cb, x + 1)
                     sourceOrigin:MTLOriginMake(extra(cb, x + 2), extra(cb, x + 3), extra(cb, x + 4))
                       sourceSize:MTLSizeMake(extra(cb, x + 5), extra(cb, x + 6), extra(cb, x + 7))
                        toTexture:twinOf((__bridge id)c.obj2)
                 destinationSlice:extra(cb, x + 8)
                 destinationLevel:extra(cb, x + 9)
                destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 11))];
            break;
        }
        case OpCopyBuffer:
            range((__bridge RMBuffer *)c.obj, c.c, extra(cb, c.x + 1));
            range((__bridge RMBuffer *)c.obj2, extra(cb, c.x), extra(cb, c.x + 1), true);
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                     sourceOffset:c.c
                         toBuffer:twinOf((__bridge id)c.obj2)
                destinationOffset:extra(cb, c.x)
                             size:extra(cb, c.x + 1)];
            break;
        case OpCopyBufferToTexture: {
            const uint64_t x = c.x, o = extra(cb, x + 8);
            {  // the rows it reads: bytesPerRow apart, the last one's texels (a whole buffer where the texture is not the raw
                // path's: its texel size unknown here)
                RMBuffer *b = (__bridge RMBuffer *)c.obj;
                RMTexture *t = (__bridge RMTexture *)c.obj2;
                const uint64_t rows = extra(cb, x + 4) * std::max<uint64_t>(extra(cb, x + 5), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x), extra(cb, x + 1) * (rows - 1) + extra(cb, x + 3) * t->_raw.bpp);
                else
                    use(c.obj);
            }
            [enc copyFromBuffer:twinOf((__bridge id)c.obj)
                       sourceOffset:extra(cb, x)
                  sourceBytesPerRow:extra(cb, x + 1)
                sourceBytesPerImage:extra(cb, x + 2)
                         sourceSize:MTLSizeMake(extra(cb, x + 3), extra(cb, x + 4), extra(cb, x + 5))
                          toTexture:twinOf((__bridge id)c.obj2)
                   destinationSlice:extra(cb, x + 6)
                   destinationLevel:extra(cb, x + 7)
                  destinationOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 9))];
            break;
        }
        case OpCopyTextureToBuffer: {
            const uint64_t x = c.x, o = extra(cb, x + 2);
            {  // the rows it writes, cleaned and invalidated (a whole buffer where the texel size is unknown here)
                RMTexture *t = (__bridge RMTexture *)c.obj;
                RMBuffer *b = (__bridge RMBuffer *)c.obj2;
                const uint64_t rows = extra(cb, x + 5) * std::max<uint64_t>(extra(cb, x + 6), 1);
                if ([(id)b isKindOfClass:[RMBuffer class]] && [(id)t isKindOfClass:[RMTexture class]] && t->_raw.bpp &&
                    rows)
                    range(b, extra(cb, x + 7), extra(cb, x + 8) * (rows - 1) + extra(cb, x + 4) * t->_raw.bpp, true);
                else
                    use(c.obj2);
            }
            [enc copyFromTexture:twinOf((__bridge id)c.obj)
                             sourceSlice:extra(cb, x)
                             sourceLevel:extra(cb, x + 1)
                            sourceOrigin:MTLOriginMake(uint32_t(o), o >> 32, extra(cb, x + 3))
                              sourceSize:MTLSizeMake(extra(cb, x + 4), extra(cb, x + 5), extra(cb, x + 6))
                                toBuffer:use(c.obj2)
                       destinationOffset:extra(cb, x + 7)
                  destinationBytesPerRow:extra(cb, x + 8)
                destinationBytesPerImage:extra(cb, x + 9)];
            break;
        }
        case OpMipmaps: [enc generateMipmapsForTexture:twinOf((__bridge id)c.obj)]; break;
        case OpInvocation: {
            NSInvocation *inv = (__bridge NSInvocation *)c.obj;
            for (NSUInteger i = 2; i < inv.methodSignature.numberOfArguments;
                 i++) {  // buffers it names get cleaned too
                const char *t = [inv.methodSignature getArgumentTypeAtIndex:i];
                if (*t != '@') continue;
                __unsafe_unretained id arg = nil;
                [inv getArgument:&arg atIndex:i];
                if ([arg isKindOfClass:[RMBuffer class]]) buffers.insert((RMBuffer *)arg);
            }
            replayInvocation(inv, c.stage == 0xff ? m : enc);
            break;
        }
        case OpSignalEvent: [m encodeSignalEvent:(__bridge id<MTLEvent>)c.obj value:c.c]; break;
        case OpWaitEvent:
            // a value raw work submitted before signals: the BeforeTask acquire orders this command buffer after it on the
            // GPU (the raw path signals its events from the CPU once complete, which m2v would wait for on the CPU)
            if (!(gRM->gpuOrder && signalled(gRM->rawSignals, c.obj) >= c.c))
                [m encodeWaitForEvent:(__bridge id<MTLEvent>)c.obj value:c.c];
            break;
        case OpDrawIndexed: {
            // its indices, and the vertices up to the largest (a restart reads none), as the raw path's indexed draw cleans
            RMBuffer *ib = [(__bridge id)c.obj isKindOfClass:[RMBuffer class]] ? (__bridge RMBuffer *)c.obj : nil;
            const uint32_t size = c.b == MTLIndexTypeUInt16 ? 2 : 4;
            if (ib && pipe && pipe->_raw && !extra(cb, c.x + 1) && c.c + uint64_t(c.a) * size <= ib->_raw.length) {
                range(ib, c.c, uint64_t(c.a) * size);
                uint32_t most = 0;
                for (uint32_t i = 0; i < c.a; i++) {
                    uint32_t v = 0;
                    memcpy(&v, ib->_raw.cpu + c.c + uint64_t(i) * size, size);
                    if (v != (size == 2 ? 0xffffu : 0xffffffffu)) most = std::max(most, v);
                }
                drawn(0, c.a ? most + 1 : 0);
            } else {
                if (ib) buffers.insert(ib);
                for (auto &stage : bound)
                    for (RMBuffer *b : stage)
                        if (b) buffers.insert(b);
            }
        }
            [enc drawIndexedPrimitives:MTLPrimitiveType(c.stage)
                            indexCount:c.a
                             indexType:MTLIndexType(c.b)
                           indexBuffer:twinOf((__bridge id)c.obj)
                     indexBufferOffset:c.c
                         instanceCount:extra(cb, c.x)
                            baseVertex:NSInteger(extra(cb, c.x + 1))
                          baseInstance:extra(cb, c.x + 2)];
            break;
        case OpVisibility: [enc setVisibilityResultMode:MTLVisibilityResultMode(c.a) offset:c.c]; break;
        }
    }
    if (enc) [enc endEncoding];
    // what the CPU wrote that it reads, to memory (above): the ranges, then the whole buffers
    uint64_t cleaned = 0;
    for (const Range &r : ranges) {
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(r.at) & ~uintptr_t(63); a < uintptr_t(r.at) + r.bytes; a += 64) {
            if (r.flush)
                __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
            else
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
        }
#endif
        cleaned += r.bytes;
    }
    for (RMBuffer *b : buffers) {
        if (!b) continue;
#if defined(__aarch64__)
        for (uintptr_t a = uintptr_t(b->_raw.cpu) & ~uintptr_t(63); a < uintptr_t(b->_raw.cpu) + b->_raw.length;
             a += 64)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
        cleaned += b->_raw.length;
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    gRM->stats.replayClean += cleaned;
    return m;
}

// ---- the tape onto the raw path ----
// false: something in it the raw path cannot do after all (*why); nothing of it was submitted then
static bool encodeRaw(RMCommandBuffer *cb, const char **why)
{
    raw::Device &d = *gRM->raw;
    raw::CommandBuffer *rc = d.begin();
    raw::RenderEncoder *re = nullptr;
    raw::BlitEncoder *be = nullptr;
    auto fail = [&](RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0) {
        *why = w;
        hit(cb, p, a, b);
        rc->abandon();
        return false;
    };
    // a draw the encoder refused: its predicate and values (raw_gpu.h Refusal)
    auto refused = [&]() {
        static const RMPred map[] = {PredEncPassRender,   PredEncIndexedRange, PredEncBaseInstance,
                                     PredEncColorFormats, PredEncUboOffset,    PredEncNoTic};
        const raw::Refusal &f = rc->refusal;
        hit(cb, f.id < sizeof map / sizeof *map ? map[f.id] : PredEncPassRender, f.a,
            uint64_t(f.b) | uint64_t(f.c) << 32);
        rc->abandon();
        return false;
    };
    // the planar textures it samples: their images from their planes first, once (m2v converts at each encoder that uses one;
    // nothing between this command buffer's passes writes the planes, refused below), then the passes, whose prologue's full
    // barrier orders the copies before their reads. A sampler that clamps to zero or to the border reads a 4:4:4 code texture
    // as clamp to edge (m2v's address map), which the raw path does not derive: such a command buffer is m2v's.
    {
        std::vector<RMTexture *> planar;
        std::vector<RMSampler *> samplers;
        auto isPlanar = [](void *o) { return o && ((__bridge RMTexture *)o)->_raw.addressMap; };
        for (const Cmd &c : cb->_tape) {
            // Metal's copies of a planar texture move its planes' bytes, not the image: m2v's
            if ((c.op == OpCopyTexture && (isPlanar(c.obj) || isPlanar(c.obj2))) ||
                (c.op == OpCopyBufferToTexture && isPlanar(c.obj2)) ||
                (c.op == OpCopyTextureToBuffer && isPlanar(c.obj)))
                return fail(PredEncPlanarCopy, "a copy to or from a planar texture", c.op);
            if (c.op == OpTexture && c.obj) {
                RMTexture *t = (__bridge RMTexture *)c.obj;
                if (t->_raw.addressMap && std::find(planar.begin(), planar.end(), t) == planar.end())
                    planar.push_back(t);
            } else if (c.op == OpSampler && c.obj) {
                RMSampler *sm = (__bridge RMSampler *)c.obj;
                if (std::find(samplers.begin(), samplers.end(), sm) == samplers.end()) samplers.push_back(sm);
            }
        }
        if (!planar.empty()) {
            // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
            for (RMTexture *t : planar)
                for (RMSampler *sm : samplers)
                    if (!d.samplerVariant(sm->_raw, t->_raw.addressMap))
                        return fail(PredEncPlanarClamp, "no sampler header for an address map");
            // 4:4:4 code textures: two copy-engine remaps each
            bool copies = false;
            for (RMTexture *t : planar) copies |= t->_raw.planar;
            if (copies) {
                raw::BlitEncoder *conv = rc->blit();
                for (RMTexture *t : planar)
                    if (t->_raw.planar) conv->convertPlanar(t->_raw);
                conv->end();
            }
            // 4:2:0 textures: a pass of the pack's conversion state each, into the image the draws sample
            for (RMTexture *t : planar) {
                raw::Texture &x = t->_raw;
                if (!x.ycbcr) continue;
                raw::PassDesc p;
                p.color[0].texture = &x;
                p.color[0].load = raw::ColorAttachment::DontCare;
                p.color[0].store = true;
                p.count = 1;
                raw::RenderEncoder *conv = rc->render(p);
                if (!conv) return fail(PredEncPassRender, "a 4:2:0 texture's image the raw path cannot render");
                conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
                conv->setBytes(2, 0, x.shape, sizeof x.shape);
                conv->setTexture(0, x.luma);
                conv->setTexture(1, x.chroma);
                conv->setViewport(0, 0, x.width, x.height, 0, 1);
                conv->setScissor(0, 0, x.width, x.height);
                if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why)) return refused();
                conv->end();
            }
        }
    }
    for (const Cmd &c : cb->_tape) {
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            raw::PassDesc p;
            for (unsigned j = 0; j < 8; j++) {
                MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[j];
                RMTexture *t = (RMTexture *)a.texture;
                if (!t) continue;
                if (t->_raw.planar) return fail(PredEncPlanarRT, "a planar texture as a render target", j);
                p.color[j].texture = &t->_raw;
                p.color[j].load = a.loadAction == MTLLoadActionClear  ? raw::ColorAttachment::Clear
                                  : a.loadAction == MTLLoadActionLoad ? raw::ColorAttachment::Load
                                                                      : raw::ColorAttachment::DontCare;
                p.color[j].store =
                    a.storeAction == MTLStoreActionStore || a.storeAction == MTLStoreActionStoreAndMultisampleResolve;
                const MTLClearColor k = a.clearColor;
                p.color[j].clear[0] = float(k.red);
                p.color[j].clear[1] = float(k.green);
                p.color[j].clear[2] = float(k.blue);
                p.color[j].clear[3] = float(k.alpha);
                p.count = j + 1;
            }
            p.width = uint32_t(std::min<NSUInteger>(pass.renderTargetWidth, UINT32_MAX));
            p.height = uint32_t(std::min<NSUInteger>(pass.renderTargetHeight, UINT32_MAX));
            if (!(re = rc->render(p))) return fail(PredEncPassRender, "a pass the raw path cannot render to", p.count);
            break;
        }
        case OpBlit: be = rc->blit(); break;
        case OpEnd:
            if (re) re->end();
            if (be) be->end();
            re = nullptr;
            be = nullptr;
            break;
        case OpPipeline: re->setPipeline(((__bridge RMPipeline *)c.obj)->_raw); break;
        case OpBytes: re->setBytes(c.stage, c.index, cb->_bytes.data() + c.a, c.b); break;
        case OpBuffer:
            re->setBuffer(c.stage, c.index, c.obj ? &((__bridge RMBuffer *)c.obj)->_raw : nullptr, c.c);
            break;
        case OpBufferOffset: re->setBufferOffset(c.stage, c.index, c.c); break;
        case OpTexture:
            if (c.stage == 2) re->setTexture(c.index, c.obj ? &((__bridge RMTexture *)c.obj)->_raw : nullptr);
            break;
        case OpSampler:
            if (c.stage == 2) re->setSampler(c.index, c.obj ? &((__bridge RMSampler *)c.obj)->_raw : nullptr);
            break;
        case OpScissor: re->setScissor(c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c)); break;
        case OpViewport:
            re->setViewport(extraDouble(cb, c.x), extraDouble(cb, c.x + 1), extraDouble(cb, c.x + 2),
                            extraDouble(cb, c.x + 3), extraDouble(cb, c.x + 4), extraDouble(cb, c.x + 5));
            break;
        case OpDraw:
            if (!re->draw(c.stage, c.a, c.b, uint32_t(c.c >> 32), uint32_t(c.c), why)) return refused();
            break;
        case OpBarrier: re->barrier(); break;
        case OpDrawIndexed:
            if (!re->drawIndexed(c.stage, c.a, c.b == MTLIndexTypeUInt16 ? 2 : 4, &((__bridge RMBuffer *)c.obj)->_raw,
                                 c.c, why))
                return refused();
            break;
        case OpCopyTexture: {
            RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
            const uint64_t x = c.x, o = extra(cb, x + 10);
            const uint64_t sx = extra(cb, x + 2), sy = extra(cb, x + 3), w = extra(cb, x + 5), h = extra(cb, x + 6);
            const uint64_t dx = uint32_t(o), dy = o >> 32;
            be->copyRect(s->_raw.va + sy * s->_raw.pitch + sx * s->_raw.bpp, s->_raw.pitch,
                         t->_raw.va + dy * t->_raw.pitch + dx * t->_raw.bpp, t->_raw.pitch, uint32_t(w), uint32_t(h),
                         s->_raw.bpp);
            break;
        }
        case OpCopyBuffer: {
            RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
            const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
            rc->clean(s->_raw.cpu + c.c, n);
            rc->clean(t->_raw.cpu + dof, n, true);
            be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
            break;
        }
        case OpCopyBufferToTexture: {
            RMBuffer *s = (__bridge RMBuffer *)c.obj;
            RMTexture *t = (__bridge RMTexture *)c.obj2;
            const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1);
            const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4);
            if (row & 31 && h > 1)
                return fail(PredEncBufToTexRow, "a buffer-to-texture copy whose rows are not 32-byte aligned", row, w);
            rc->clean(s->_raw.cpu + so, row * (h - 1) + w * t->_raw.bpp);
            be->copyRect(s->_raw.va + so, uint32_t(row),
                         t->_raw.va + (o >> 32) * t->_raw.pitch + uint32_t(o) * t->_raw.bpp, t->_raw.pitch, uint32_t(w),
                         uint32_t(h), t->_raw.bpp);
            break;
        }
        case OpCopyTextureToBuffer: {
            RMTexture *s = (__bridge RMTexture *)c.obj;
            RMBuffer *t = (__bridge RMBuffer *)c.obj2;
            const uint64_t x = c.x, o = extra(cb, x + 2), w = extra(cb, x + 4), h = extra(cb, x + 5),
                           dof = extra(cb, x + 7), row = extra(cb, x + 8);
            rc->clean(t->_raw.cpu + dof, row * (h - 1) + w * s->_raw.bpp, true);
            be->copyRect(s->_raw.va + (o >> 32) * s->_raw.pitch + uint32_t(o) * s->_raw.bpp, s->_raw.pitch,
                         t->_raw.va + dof, uint32_t(row), uint32_t(w), uint32_t(h), s->_raw.bpp);
            break;
        }
        default:  // states, fences, events, resources the raw path needs nothing for
            break;
        }
    }
    if (re) re->end();
    if (be) be->end();
    uint32_t seq = 0;
    if (!rc->commit(seq)) return fail(PredEncKernel, "the kernel refused the submission");
    cb->_seqno = seq;
    gRM->stats.draws += rc->draws;
    gRM->stats.passes += rc->passes;
    gRM->stats.copies += rc->blits;
    return true;
}

// The textures and buffers a command buffer wrote (render targets, copy destinations, and for m2v anything a compute encoder
// bound): their CPU lines are invalidated before the CPU next reads them.
static void markWritten(RMCommandBuffer *cb)
{
    bool compute = false;
    for (const Cmd &c : cb->_tape) {
        switch (c.op) {
        case OpRender: {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
            for (unsigned j = 0; j < 8; j++)
                if (RMTexture *t = (RMTexture *)pass.colorAttachments[j].texture)
                    if ([t isKindOfClass:[RMTexture class]]) t->_gpuDirty = true;
            break;
        }
        case OpCompute: compute = true; break;
        case OpBlit: compute = false; break;
        case OpEnd: compute = false; break;
        case OpCopyTexture:
        case OpCopyBufferToTexture:
        case OpMipmaps:
            if (RMTexture *t = (__bridge RMTexture *)(c.op == OpMipmaps ? c.obj : c.obj2)) t->_gpuDirty = true;
            break;
        case OpCopyBuffer:
        case OpCopyTextureToBuffer: ((__bridge RMBuffer *)c.obj2)->_gpuDirty = true; break;
        case OpBuffer:
        case OpTexture:
            if (compute && c.obj) {
                id o = (__bridge id)c.obj;
                if ([o isKindOfClass:[RMBuffer class]]) ((RMBuffer *)o)->_gpuDirty = true;
                if ([o isKindOfClass:[RMTexture class]]) ((RMTexture *)o)->_gpuDirty = true;
            }
            break;
        case OpInvocation: {  // compute and other recorded calls: every buffer and texture they name
            NSInvocation *inv = (__bridge NSInvocation *)c.obj;
            for (NSUInteger i = 2; i < inv.methodSignature.numberOfArguments; i++) {
                if (*[inv.methodSignature getArgumentTypeAtIndex:i] != '@') continue;
                __unsafe_unretained id arg = nil;
                [inv getArgument:&arg atIndex:i];
                if ([arg isKindOfClass:[RMBuffer class]]) ((RMBuffer *)arg)->_gpuDirty = true;
                if ([arg isKindOfClass:[RMTexture class]]) ((RMTexture *)arg)->_gpuDirty = true;
            }
            break;
        }
        default: break;
        }
    }
}

// Scheduled: handlers in order on the device's handler queue (never inside commit), then waitUntilScheduled returns.
// The committed command buffers that wait for a shared event, until scheduled (heldBehindEvent).
static std::mutex gHoldersLock;
static std::set<RMCommandBuffer *> gHolders;

static bool waitsForEvent(RMCommandBuffer *cb)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpWaitEvent) return true;
    return false;
}

// Whether a committed command buffer not yet scheduled waits for a shared event value nothing reached yet.
static bool heldBehindEvent()
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    for (RMCommandBuffer *cb : gHolders)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpWaitEvent && [(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] < c.c) return true;
    return false;
}

static void unhold(RMCommandBuffer *cb)
{
    std::lock_guard<std::mutex> l(gHoldersLock);
    gHolders.erase(cb);
}

static void scheduled(RMCommandBuffer *cb)
{
    unhold(cb);
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_scheduledSent) return;  // once (eventHeld: at its commit; then its submission's)
        cb->_scheduledSent = true;
        if (cb->_status == MTLCommandBufferStatusCommitted) cb->_status = MTLCommandBufferStatusScheduled;
        handlers = [cb->_scheduledHandlers copy];
        [cb->_scheduledHandlers removeAllObjects];
        cb->_gpuStart = hostSeconds();
    }
    dispatch_async(gRM->handlers, ^{
        for (MTLCommandBufferHandler h in handlers) h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_scheduledDone = true;
            // a handler added meanwhile ran at once (addScheduledHandler)
        }
        cb->_changed.notify_all();
    });
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
static void finish(RMCommandBuffer *cb, NSError *error)
{
    unhold(cb);
    markWritten(cb);
    if (!cb->_m2v)
        for (const Cmd &c : cb->_tape)
            if (c.op == OpSignalEvent) [(id<MTLSharedEvent>)(__bridge id)c.obj setSignaledValue:c.c];
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        cb->_status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
        cb->_error = error;
        cb->_gpuEnd = hostSeconds();
        handlers = [cb->_completedHandlers copy];
        [cb->_completedHandlers removeAllObjects];
        cb->_completedSent = true;
        cb->_scheduledSent = true;  // a completed command buffer is scheduled; its own scheduled() runs no more
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler h in handlers) h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_completedDone = true;
            cb->_scheduledDone = true;
        }
        cb->_changed.notify_all();
        cb->_owners.clear();  // the resources may die now: no submission uses them any more
        cb->_owners.shrink_to_fit();
    };
    dispatch_queue_t q = cb->_queue ? cb->_queue->_completionQueue : nil;
    if (q)
        dispatch_async(q, run);
    else
        run();
}

static void commitBuffer(RMCommandBuffer *cb);
@implementation RMDevice
- (void)rmCommit:(RMCommandBuffer *)cb
{
    commitBuffer(cb);
}
// Alignments Core Animation lays its memory out by (QuartzCore 21H461), answered for what the raw path takes as it is; m2v takes
// any multiple of 16 (its own answers), so the m2v twins of the same memory are unchanged in what they hold:
//  - its buffer pool's suballocations (CA::OGL::MetalBufferPool::get_buffer: max(this, the argument's)): a uniform buffer the
//    raw path binds at its address (a constant buffer's is a multiple of 256 on GM20B; m2v's residual is not in the pack);
//  (not iosurfaceReadOnlyTextureAlignmentBytes: QuartzCore makes it the render server's needed IOSurface row alignment,
//  CADeviceSetMinimumAlignment, and hands it to every client, CA::Context::connect_remote; MetalContext::update_surface then
//  refuses every surface whose rows miss it, "Row bytes (N) not 128 aligned", and the layer draws nothing (r7p5: 3347 refusals
//  in backboardd, Safari images and widgets blank). The m2v device's answer, 16, stands; rows the raw path cannot take stay
//  m2v's: its texture header takes rows of 32, a pitch render target rows of 128.)
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of this,
//    else it copies them into a texture of its own): the texture header's 32;
//  - the rows of its CPU readbacks (CAMetalTextureGetBytesPerRow).
- (NSUInteger)minConstantBufferAlignmentBytes
{
    return 256;
}
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes
{
    return 32;
}
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    const NSUInteger a = [gRM->fallback minimumLinearTextureAlignmentForPixelFormat:format];
    return a ? std::max<NSUInteger>(a, 32) : a;
}
// (every other selector: the m2v device's own method, which the device class inherits)
- (id<MTLCommandQueue>)newCommandQueue
{
    RMQueue *q = [RMQueue new];
    q->_device = self;
    return (id<MTLCommandQueue>)q;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
    (void)n;
    return [self newCommandQueue];
}
- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(id)d
{
    (void)d;
    return [self newCommandQueue];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o
{
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw)) {  // the pool could not grow: m2v's buffer (its users go to m2v)
        once([NSString stringWithFormat:@"no raw memory for a %lu-byte buffer: m2v's", (unsigned long)n]);
        if (!predLogged(PredCallBufForeign)) logPred(PredCallBufForeign, 0, n, @"(made: the pool had no room)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithLength:n options:o];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    return (id<MTLBuffer>)b;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (b && p && n) {
        memcpy(((RMBuffer *)b)->_raw.cpu, p, n);  // the draws that read it clean the lines they read
    }
    return b;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p
                                   length:(NSUInteger)n
                                  options:(MTLResourceOptions)o
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw)) {  // (not page-aligned, or refused): m2v's, as today
        if (!predLogged(PredCallBufForeign))
            logPred(PredCallBufForeign, uintptr_t(p) & 0xfff, n, @"(made: a no-copy buffer the raw path refused)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithBytesNoCopy:p length:n options:o deallocator:deallocator];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    b->_deallocator = [deallocator copy];
    return (id<MTLBuffer>)b;
}
- (id<MTLTexture>)wrapTwin:(id<MTLTexture>)twin descriptor:(MTLTextureDescriptor *)d
{
    if (!twin) return nil;
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_twin = twin;
    t->_kind = RMTextureTwinOnly;
    t->_descriptor = [d copy];
    return (id<MTLTexture>)t;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    if (!rawShape(d)) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w) refuseTexture(w, PredTexShape, rawShapeTerm(d), d.pixelFormat, @"twin-only", nil);
        return (id<MTLTexture>)w;
    }
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    if (d.storageMode == MTLStorageModeMemoryless) {
        t->_kind = RMTextureMemoryless;
        t->_rawOk = gRM->raw->createMemoryless(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw);
    } else {
        t->_kind = RMTexturePool;
        t->_rawOk = gRM->raw->createTexture(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw);
    }
    if (!t->_rawOk) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w) refuseTexture(w, PredTexPoolAlloc, d.width, d.height, @"twin-only (pool)", nil);
        return (id<MTLTexture>)w;
    }
    return (id<MTLTexture>)t;
}
// RGB8_444_2P (542) over plane 0 of a two-plane 4:4:4 surface of one byte a luma texel and two a chroma pair (CoreVideo's
// '_RB4', the widget and poster snapshots Core Animation samples): Metal reads its codes, red Cr, green Y, blue Cb, alpha 1
// (m2v ycbcr.rs), which the raw path holds in an RGBA8 image it writes from the planes with the copy engine before the work that
// samples it (raw::BlitEncoder::convertPlanar). A 2D texture of the surface's size, one level, no view, no rotation, sampled.
static bool planar444(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.pixelFormat == kRGB8_444_2P && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView)) &&
           IOSurfaceGetWidthOfPlane(s, 0) == d.width && IOSurfaceGetHeightOfPlane(s, 0) == d.height &&
           IOSurfaceGetWidthOfPlane(s, 1) == d.width && IOSurfaceGetHeightOfPlane(s, 1) == d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 1 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 2;
}
// YCBCR8_420_2P (500), YCBCR8_420_2P_sRGB (520) or RGB8_420_2P (540) over plane 0 of a CoreVideo '420v' or '420f' surface
// (decoded photos and video frames, the photo wallpaper): Metal reads it converted by the surface's range and matrix (500), that
// through the sRGB curve (520), or its codes (540), each with its chroma filtered 9/3/3/1 (m2v ycbcr.rs), which the raw path
// draws into an image of its own with the pack's conversion state (pack/ycbcr.metal, exact against Metal's measured reads:
// test/ycbcrcheck.mm) before the work that samples it. The surface's planes where m2v takes them (16-byte offsets and rows,
// the chroma plane at least the texture's half), and a matrix it takes (none or BT.601, BT.709, BT.2020, SMPTE 240M as BT.709).
// `shape`: the conversion's fragment buffer (width, height, full range, then red's, green's and blue's kb and kr).
static bool ycbcr420(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, uint32_t shape[9])
{
    const NSUInteger f = d.pixelFormat;
    if ((f != 500 && f != 520 && f != 540) || plane != 0 || IOSurfaceGetPlaneCount(s) != 2) return false;
    const uint32_t fourcc = IOSurfaceGetPixelFormat(s);
    const bool full = fourcc == '420f';
    if (!full && fourcc != '420v') return false;
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const size_t w = d.width, h = d.height;
    if (d.textureType != MTLTextureType2D || d.mipmapLevelCount != 1 || d.sampleCount != 1 || turned ||
        (d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView)) ||
        IOSurfaceGetWidthOfPlane(s, 0) != w || IOSurfaceGetHeightOfPlane(s, 0) != h ||
        IOSurfaceGetWidthOfPlane(s, 1) < (w + 1) / 2 || IOSurfaceGetHeightOfPlane(s, 1) < (h + 1) / 2 ||
        IOSurfaceGetBytesPerElementOfPlane(s, 0) != 1 || IOSurfaceGetBytesPerElementOfPlane(s, 1) != 2)
        return false;
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t p : {0, 1})
        if (((uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, p)) - base) & 15) ||
            (IOSurfaceGetBytesPerRowOfPlane(s, p) & 15))
            return false;
    int matrix =
        0;  // m2v ycbcr.rs: none and ITU_R_601_4 BT.601, ITU_R_709_2 and SMPTE_240M_1995 BT.709, ITU_R_2020 BT.2020
    if (CFTypeRef value = IOSurfaceCopyValue(s, CFSTR("IOSurfaceYCbCrMatrix"))) {
        char name[32] = "";
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, name, sizeof name, kCFStringEncodingUTF8);
        CFRelease(value);
        if (!strcmp(name, "ITU_R_709_2") || !strcmp(name, "SMPTE_240M_1995"))
            matrix = 1;
        else if (!strcmp(name, "ITU_R_2020"))
            matrix = 2;
        else if (strcmp(name, "ITU_R_601_4"))
            return false;
    }
    // m2v ycbcr_convert.rs `coefficients`: each channel's (kb, kr) of the range and matrix
    static const int32_t k[2][3][6] = {
        {{-9, 13123, -3218, -6686, 16591, 9}, {-1, 14743, -1753, -4383, 17373, 1}, {0, 13806, -1540, -5349, 17614, -1}},
        {{-8, 11483, -2816, -5850, 14518, 8},
         {-1, 12900, -1534, -3835, 15201, 1},
         {0, 12080, -1348, -4680, 15412, -1}}};
    shape[0] = uint32_t(w);
    shape[1] = uint32_t(h);
    shape[2] = full;
    for (int i = 0; i < 6; i++) shape[3 + i] = uint32_t(k[full][matrix][i]);
    return true;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    if (!s) return nil;
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTextureSurface;
    t->_surface = (IOSurfaceRef)CFRetain(s);
    t->_plane = plane;
    uint32_t shape[9];
    if (ycbcr420(d, s, plane, shape)) {  // a 4:2:0 YCbCr texture: an image the pack's conversion state draws
        void *base = IOSurfaceGetBaseAddress(s);
        const uintptr_t at = uintptr_t(base);
        t->_rawOk = gRM->raw->createYcbcrTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), uint32_t(d.width), uint32_t(d.height),
            uint32_t(d.pixelFormat), shape, t->_raw);
    } else if (planar444(d, s, plane)) {  // RGB8_444_2P over a 4:4:4 two-plane surface: an RGBA8 image of its codes
        void *base = IOSurfaceGetBaseAddress(s);
        const uintptr_t at = uintptr_t(base);
        t->_rawOk = gRM->raw->createPlanarTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), uint32_t(d.width), uint32_t(d.height), t->_raw);
    } else if (rawShape(d) && !plane && IOSurfaceGetPlaneCount(s) <= 1) {
        // a surface without planes, or one described by a single plane (CoreVideo's chunky surfaces): its plane's bytes
        void *base = IOSurfaceGetBaseAddress(s);  // valid while the surface lives (the import retains it)
        const bool one = IOSurfaceGetPlaneCount(s) == 1;
        const uint64_t offset = one ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - uintptr_t(base) : 0;
        const size_t pitch = one ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(pitch),
                                                   uint32_t(d.pixelFormat), t->_raw);
    }
    if (!t->_rawOk) {  // a surface the raw path does not take: m2v's texture over it, made now (it may refuse: nil, as today)
        // which term (createSurfaceTexture's, recomputed here: a creation, not the hot path)
        const size_t planes = IOSurfaceGetPlaneCount(s),
                     pitch = planes == 1 ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        NSString *extra =
            [NSString stringWithFormat:@"surface %ux%u '%c%c%c%c' bytesPerRow %lu planes %lu plane %lu alloc %lu "
                                       @"base&0xfff 0x%lx",
                                       unsigned(IOSurfaceGetWidth(s)), unsigned(IOSurfaceGetHeight(s)),
                                       char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc),
                                       (unsigned long)pitch, (unsigned long)planes, (unsigned long)plane,
                                       (unsigned long)IOSurfaceGetAllocSize(s), (unsigned long)(base & 0xfff)];
        if (planar444(d, s, plane))
            refuseTexture(t, PredTexPlanar, d.width, d.height, @"surface", extra);
        else if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"surface", extra);
        else if (plane || planes > 1)
            refuseTexture(t, PredTexSurfPlane, plane, planes, @"surface", extra);
        else if ((pitch & 31) || pitch < packed)
            refuseTexture(t, PredTexSurfPitch, pitch, packed, @"surface", extra);
        else if (uint64_t(pitch) * d.height > IOSurfaceGetAllocSize(s))
            refuseTexture(t, PredTexSurfBounds, uint64_t(pitch) * d.height, IOSurfaceGetAllocSize(s), @"surface",
                          extra);
        else if (base & (size_t(getpagesize()) - 1))
            refuseTexture(t, PredTexSurfBase, base & 0xfff, 0, @"surface", extra);
        else
            refuseTexture(t, PredTexSurfImport, 1, 0, @"surface", extra);
        if (![t rmTwin]) return nil;
    }
    return (id<MTLTexture>)t;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    raw::SamplerDesc s;
    s.minFilter = uint8_t(d.minFilter);
    s.magFilter = uint8_t(d.magFilter);
    s.mipFilter = uint8_t(d.mipFilter);
    s.sAddress = uint8_t(d.sAddressMode);
    s.tAddress = uint8_t(d.tAddressMode);
    s.rAddress = uint8_t(d.rAddressMode);
    s.lodMin = d.lodMinClamp;
    s.lodMax = d.lodMaxClamp;
    RMSampler *r = [RMSampler new];
    r->_device = self;
    r->_descriptor = [d copy];
    r.label = d.label;
    // what the raw sampler header does not express (anisotropy, compare, border colours, unnormalised coordinates, LOD average):
    // the m2v twin is the sampler, and the raw path refuses the draws that use it
    const bool rawOk = d.maxAnisotropy <= 1 && d.normalizedCoordinates &&
                       d.compareFunction == MTLCompareFunctionNever &&
                       d.sAddressMode != MTLSamplerAddressModeClampToBorderColor &&
                       d.tAddressMode != MTLSamplerAddressModeClampToBorderColor && !d.lodAverage;
    if (!rawOk || !gRM->raw->createSampler(s, r->_raw)) {
        if (!predLogged(PredCallSamplerForeign))
            logPred(
                PredCallSamplerForeign, d.maxAnisotropy, d.compareFunction,
                [NSString
                    stringWithFormat:@"(made: normalized %d address %lu/%lu border %lu lodAverage %d header slot %d)",
                                     (int)d.normalizedCoordinates, (unsigned long)d.sAddressMode,
                                     (unsigned long)d.tAddressMode, (unsigned long)d.borderColor, (int)d.lodAverage,
                                     (int)rawOk]);
        if (![r rmTwin]) return nil;
        RMTwinObject *o =
            [RMTwinObject new];  // a sampler the raw path cannot use: a draw with it goes to m2v (not an RMSampler)
        o->_device = self;
        o->_twin = r->_twin;
        return (id<MTLSamplerState>)o;
    }
    return (id<MTLSamplerState>)r;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    id twin = [gRM->fallback newDepthStencilStateWithDescriptor:d];
    if (!twin) return nil;
    RMTwinObject *o = [RMTwinObject new];
    o->_device = self;
    o->_twin = twin;
    o.label = d.label;
    return (id<MTLDepthStencilState>)o;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithURL:url error:error], url.path);
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithFile:path error:error], path);
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithData:data error:error], nil);
}
- (id<MTLLibrary>)newDefaultLibrary
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibrary], nil);
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibraryWithBundle:bundle error:error],
                                       [bundle.bundlePath stringByAppendingString:@"/default.metallib"]);
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithSource:source options:options error:error],
                                       nil);
}
// A descriptor whose functions are this device's: a copy over their twins, for the m2v device
static MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)
{
    MTLRenderPipelineDescriptor *t = [d copy];
    t.vertexFunction = twinOf(d.vertexFunction);
    t.fragmentFunction = twinOf(d.fragmentFunction);
    return t;
}
- (RMPipeline *)pipelineFor:(MTLRenderPipelineDescriptor *)d twin:(id<MTLRenderPipelineState>)twin
{
    RMPipeline *p = [RMPipeline new];
    p->_device = self;
    p->_twin = twin;
    p.label = d.label;
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    p->_quartzCore = vf && ff && vf->_quartzCore && ff->_quartzCore;
    if (p->_quartzCore) {
        NSString *key = raw_metal_pipeline_key(d);
        const char *k = key.UTF8String;
        p->_raw = gRM->raw->pipeline(raw::Pack::fnv(k, strlen(k)));
    }
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (!a.blendingEnabled) continue;
        for (MTLBlendFactor f : {a.sourceRGBBlendFactor, a.destinationRGBBlendFactor, a.sourceAlphaBlendFactor,
                                 a.destinationAlphaBlendFactor})
            if (f >= MTLBlendFactorBlendColor && f <= MTLBlendFactorOneMinusBlendAlpha) p->_constantBlend = true;
    }
    if (!p->_raw)
        once([NSString stringWithFormat:@"state not in the pack (m2v draws it)%s: %@",
                                        p->_quartzCore ? "" : " (not QuartzCore's functions: never looked up)",
                                        d.label ?: @"-"]);
    return p;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    // m2v's state over the twins is made as today (asynchronously in the render server: AsyncPipelines.h), so a command buffer
    // that has to go to m2v finds it made
    id<MTLRenderPipelineState> twin = [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                                    error:error];
    RMPipeline *p = [self pipelineFor:d twin:twin];
    if (!twin && !p->_raw) return nil;
    if (error && p->_raw) *error = nil;
    return (id<MTLRenderPipelineState>)p;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    id<MTLRenderPipelineState> twin = [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                                  options:o
                                                                               reflection:reflection
                                                                                    error:error];
    RMPipeline *p = [self pipelineFor:d twin:twin];
    if (!twin && !p->_raw) return nil;
    return (id<MTLRenderPipelineState>)p;
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                      completionHandler:^(id<MTLRenderPipelineState> twin, NSError *e) {
                                          RMPipeline *p = [me pipelineFor:copy twin:twin];
                                          handler((!twin && !p->_raw) ? nil : (id<MTLRenderPipelineState>)p, e);
                                      }];
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                     options:(MTLPipelineOption)o
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                options:o
                                      completionHandler:^(id<MTLRenderPipelineState> twin,
                                                          MTLRenderPipelineReflection *r, NSError *e) {
                                          RMPipeline *p = [me pipelineFor:copy twin:twin];
                                          handler((!twin && !p->_raw) ? nil : (id<MTLRenderPipelineState>)p, r, e);
                                      }];
}
- (id)wrapCarried:(id)twin
{
    if (!twin) return nil;
    RMTwinObject *o = [RMTwinObject new];
    o->_device = self;
    o->_twin = twin;
    return o;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error
{
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithFunction:twinOf(f) error:error]];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                             error:(NSError **)error
{
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithFunction:twinOf(f)
                                                                        options:o
                                                                     reflection:r
                                                                          error:error]];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                             options:(MTLPipelineOption)o
                                                          reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                               error:(NSError **)error
{
    MTLComputePipelineDescriptor *t = [d copy];
    t.computeFunction = twinOf(d.computeFunction);
    return [self wrapCarried:[gRM->fallback newComputePipelineStateWithDescriptor:t
                                                                          options:o
                                                                       reflection:r
                                                                            error:error]];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d
                                                               options:(MTLPipelineOption)o
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)r
                                                                 error:(NSError **)error
{
    MTLTileRenderPipelineDescriptor *t = [d copy];
    t.tileFunction = twinOf(d.tileFunction);
    return [self wrapCarried:[gRM->fallback newRenderPipelineStateWithTileDescriptor:t
                                                                             options:o
                                                                          reflection:r
                                                                               error:error]];
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    RMHeap *h = [RMHeap new];
    h->_device = self;
    h->_descriptor = [d copy];
    return (id<MTLHeap>)h;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    return [NSObject new];  // cache hints are no-ops here (encodeCacheHint...)
}
@end

// ---- commit: order between the two drivers ----
// With the kernel's Order (TegraGM20BABI.h method 14), the two drivers' command buffers are ordered on the GPU: before a raw
// submission that follows m2v work, one Order(AfterTask) makes its first entry a semaphore acquire on what NVK's channels had
// submitted; before an m2v commit that follows raw work, one Order(BeforeTask) makes NVK's channels' next submissions start with
// an acquire on the raw channel's last seqno. Neither thread waits for the GPU. The one CPU handoff left: a raw command buffer is
// submitted once the m2v ones before it are scheduled (NVK's Exec made, so the AfterTask acquire covers them) - a wait for the
// m2v runtime's own submission of work the GPU is already running, not for that work. What the GPU order cannot cover is
// ordered on the CPU, each with a one-shot line naming it: a kernel without Order, an Order the kernel refuses, a shared event
// signalled outside this device's submitted work.

static uint64_t signalled(NSMapTable *map, void *event)
{
    NSNumber *n = [map objectForKey:(__bridge id)event];
    return n ? n.unsignedLongLongValue : 0;
}

// submit lock held: the event signals `cb` makes, as submitted ones of its driver
static void noteSignals(RMCommandBuffer *cb, NSMapTable *map)
{
    for (const Cmd &c : cb->_tape)
        if (c.op == OpSignalEvent && c.c > signalled(map, c.obj)) [map setObject:@(c.c) forKey:(__bridge id)c.obj];
}

// Whether the shared event wait `i` of `cb` is covered without a CPU wait: signalled already, earlier in `cb`, or (GPU order)
// by a command buffer submitted before it - raw: the channel's own order; m2v: scheduled, so behind the AfterTask acquire.
static bool waitCovered(RMCommandBuffer *cb, size_t i)
{
    const Cmd &c = cb->_tape[i];
    for (size_t k = 0; k < i; k++)
        if (cb->_tape[k].op == OpSignalEvent && cb->_tape[k].obj == c.obj && cb->_tape[k].c >= c.c) return true;
    if ([(id<MTLSharedEvent>)(__bridge id)c.obj signaledValue] >= c.c) return true;
    return gRM->gpuOrder && (signalled(gRM->rawSignals, c.obj) >= c.c || signalled(gRM->m2vSignals, c.obj) >= c.c);
}

static bool eventsReady(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++)
        if (cb->_tape[i].op == OpWaitEvent && !waitCovered(cb, i)) return false;
    return true;
}

// How long a command buffer waits for a shared event another process signals (waitEvents).
static constexpr int64_t kEventLimitNs = 10 * NSEC_PER_SEC;

// The CPU-ordered remainder: each wait nothing submitted covers, until the event's value is reached (notified, no polling).
static void waitEvents(RMCommandBuffer *cb)
{
    for (size_t i = 0; i < cb->_tape.size(); i++) {
        if (cb->_tape[i].op != OpWaitEvent || waitCovered(cb, i)) continue;
        const Cmd &c = cb->_tape[i];
        once(@"CPU-ordered: a shared event signalled outside this device's submitted work (the host or another "
             @"process): "
             @"the command buffer waiting for it is submitted once it is reached");
        gRM->stats.cpuOrdered++;
        dispatch_semaphore_t reached = dispatch_semaphore_create(0);
        id<MTLSharedEvent> event = (__bridge id)c.obj;
        [event notifyListener:gRM->listener
                      atValue:c.c
                        block:^(id<MTLSharedEvent>, uint64_t) { dispatch_semaphore_signal(reached); }];
        // A signaller that never reaches it (a client killed or suspended mid-frame: Core Animation's per-surface events,
        // CA::OGL::shared_event_submit) holds no one past kEventLimit: the command buffer goes on as if the event were reached,
        // as Core Animation's own SharedEvent::force_complete would make it (r7n: AegirPoster died, r7o: PosterKit's scenes
        // were suspended, and this wait held every later command buffer and the render thread for good).
        if (dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW, kEventLimitNs))) {
            gRM->stats.eventsReleased++;
            os_log_error(rawLog(),
                         "raw: shared event %p (%{public}s) not reached %llu after %lld ms (at %llu): its command "
                         "buffer goes on without it",
                         (__bridge void *)event, object_getClassName(event), (unsigned long long)c.c,
                         (long long)(kEventLimitNs / 1000000), (unsigned long long)event.signaledValue);
        }
    }
}

// What a raw command buffer waits for before it is submitted: GPU order, the m2v command buffers before it scheduled; CPU
// order, completed. m2vLock held.
static bool m2vClear()
{
    return gRM->gpuOrder ? gRM->m2vUnscheduled == 0 : gRM->m2vPending == 0;
}

// The kernel refused an Order: from here on the drivers are ordered on the CPU (one line).
static void orderRefused(const char *what, int kr)
{
    once([NSString stringWithFormat:@"CPU-ordered from now on: the kernel refused Order (%s, 0x%x)", what, kr]);
    std::lock_guard<std::mutex> m(gRM->m2vLock);
    gRM->gpuOrder = false;
}

static void countDriver(int driver)
{
    RawState &r = *gRM;
    if (r.lastDriver && r.lastDriver != driver) r.stats.switches++;
    r.lastDriver = driver;
}

// Submits `cb` now: the caller holds gRM->submit and everything committed before it is submitted. false: not yet (a raw
// command buffer whose Order was refused just now waits for the m2v work before it to complete; the caller keeps it first).
// Diagnostics: each predicate `cb` hit, once (and once more when it was the only one)
static void countPreds(RMCommandBuffer *cb)
{
    const uint64_t m = cb->_preds;
    if (!m) return;
    const bool only = !(m & (m - 1));
    for (uint64_t left = m; left; left &= left - 1) {
        const int p = __builtin_ctzll(left);
        gPredCbs[p].fetch_add(1, std::memory_order_relaxed);
        if (only) gPredOnly[p].fetch_add(1, std::memory_order_relaxed);
    }
}

static bool submitLocked(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    const char *why = nullptr;
    if (cb->_reason == WhyRaw) {
        if (r.gpuOrder && r.m2vSinceRaw) {  // its first entry: an acquire on what NVK's channels submitted
            uint32_t waits = 0;
            const int kr = r.raw->orderAfterOthers(waits);
            if (kr == KERN_SUCCESS) {
                r.m2vSinceRaw = false;
                r.stats.acquires += waits;
            } else {
                orderRefused("raw after m2v", kr);
                std::lock_guard<std::mutex> m(r.m2vLock);
                if (r.m2vPending) return false;
            }
        }
        const uint64_t began = nowUs();
        if (encodeRaw(cb, &why)) {
            r.stats.encodeUs += nowUs() - began;
            r.stats.raw++;
            r.rawSinceM2v = true;
            countDriver(1);
            noteSignals(cb, r.rawSignals);
            {
                std::lock_guard<std::mutex> l(r.doneLock);
                r.inflight.push_back(cb);
            }
            r.doneChanged.notify_one();
            scheduled(cb);
            return true;
        }
        r.stats.aborted++;
        r.stats.abortUs += nowUs() - began;
        cb->_reason = WhyEncode;
        cb->_why = [NSString stringWithUTF8String:why ?: "?"];
    }
    r.stats.m2v++;
    r.stats.reasons[cb->_reason]++;
    countPreds(cb);
    {  // one line per reason kind (no string per command buffer: the predicates' own lines carry the values)
        static std::atomic<uint32_t> logged{0};
        const uint32_t bit = 1u << cb->_reason;
        if (!(logged.fetch_or(bit, std::memory_order_relaxed) & bit))
            once([NSString stringWithFormat:@"command buffer on m2v (%s): %@ [first predicate %s]",
                                            kReasonNames[cb->_reason], cb->_why, kPredNames[cb->_pred0]]);
    }
    cb->_m2v = true;
    const uint64_t replayBegan = nowUs();
    id<MTLCommandBuffer> m = r.fallback ? replay(cb) : nil;
    if (!m) {
        finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:nil]);
        return true;
    }
    // after the raw work before it: NVK's next submissions start with an acquire on the raw channel (GPU order), or that work
    // completes first (CPU order: a kernel wait on the fence interrupt)
    if (r.gpuOrder && r.rawSinceM2v) {
        uint32_t waits = 0;
        const int kr = r.raw->orderOthersAfter(waits);
        if (kr == KERN_SUCCESS)
            r.stats.acquires += waits;
        else
            orderRefused("m2v after raw", kr);
    }
    if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
        r.stats.cpuOrdered++;
        r.raw->wait(r.raw->submitted());
    }
    r.rawSinceM2v = false;
    r.m2vSinceRaw = true;
    countDriver(2);
    noteSignals(cb, r.m2vSignals);
    __block bool counted = false;  // m2vLock: it left m2vUnscheduled (scheduled, or completed without that)
    {
        std::lock_guard<std::mutex> l(r.m2vLock);
        r.m2vPending++;
        r.m2vUnscheduled++;
    }
    [m addScheduledHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted) gRM->m2vUnscheduled--;
            counted = true;
        }
        gRM->m2vChanged.notify_all();
        scheduled(cb);
    }];
    [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
        finish(cb, done.error);
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted) gRM->m2vUnscheduled--;  // a refused commit never schedules
            counted = true;
            gRM->m2vPending--;
        }
        gRM->m2vChanged.notify_all();
    }];
    [m commit];
    r.stats.replayUs += nowUs() - replayBegan;
    return true;
}

// A committed command buffer: submitted now when nothing committed before it waits and what it needs is there (raw: m2vClear,
// the shared events it waits for covered), else handed to the submit thread in order.
static void commitBuffer(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    if (waitsForEvent(cb)) {
        std::lock_guard<std::mutex> h(gHoldersLock);
        gHolders.insert(cb);
    }
    std::unique_lock<std::mutex> l(r.submit);
    bool now = r.waiting.empty();
    if (now && cb->_reason == WhyRaw) {
        std::lock_guard<std::mutex> m(r.m2vLock);
        now = m2vClear();
        if (!now && !r.gpuOrder) r.stats.cpuOrdered++;  // it waits for the m2v work before it to complete
    }
    if (now && cb->_reason == WhyRaw) now = eventsReady(cb);
    if (now && submitLocked(cb)) return;
    r.stats.deferred++;
    r.waiting.push_back(cb);
    l.unlock();
    r.waitingChanged.notify_one();
}

// The submit thread: the command buffers that had to wait, in order, each once what it waits for is there.
static void submitLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.submit);
            r.waitingChanged.wait(l, [&] { return !r.waiting.empty(); });
            cb = r.waiting.front();
        }
        if (cb->_reason == WhyRaw) {
            {
                std::unique_lock<std::mutex> m(r.m2vLock);
                if (!r.m2vChanged.wait_for(m, kWaitLimit, [&] { return m2vClear(); }))
                    waitTooLong("raw command buffer waiting for the m2v work before it, command buffer",
                                (__bridge void *)cb);
            }
            waitEvents(cb);
        }
        std::lock_guard<std::mutex> l(r.submit);
        if (!submitLocked(cb)) continue;  // CPU order from now on: it waits for the m2v work before it to complete
        r.waiting.pop_front();
    }
}

// The completion thread: sleeps in the kernel on each raw submission's fence (the interrupt), in order.
static void completionLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.doneLock);
            r.doneChanged.wait(l, [&] { return !r.inflight.empty(); });
            cb = r.inflight.front();
        }
        r.raw->wait(cb->_seqno);
        {
            std::lock_guard<std::mutex> l(r.doneLock);
            r.inflight.pop_front();
        }
        finish(cb, nil);
    }
}

// ---- the rates line ----
static struct {
    uint64_t raw, m2v, encodeUs, draws, passes, copies, deferred, aborted, reasons[WhyCount], ringWaits, lastUs,
        switches, cpuOrdered, acquires, poolStalls, eventsReleased, heldReturns, abortUs, replayUs, replayClean,
        rtUnaligned, predOnly[PredCount];
} gRates;

const void *raw_metal_texture_image(id<MTLTexture> texture, size_t *pitch)
{
    if (![(id)texture isKindOfClass:[RMTexture class]]) return nullptr;
    RMTexture *t = (RMTexture *)texture;
    if (!t->_rawOk || !t->_raw.cpu) return nullptr;
    if (pitch) *pitch = t->_raw.pitch;
    return t->_raw.cpu;
}

size_t raw_metal_rates(char *out, size_t size)
{
    if (!gRM || !size) return 0;
    Stats &s = gRM->stats;
    const uint64_t raw = s.raw.load(), m2v = s.m2v.load(), now = nowUs();
    const uint64_t c = raw - gRates.raw, f = m2v - gRates.m2v;
    if (!c && !f) return 0;
    const double secs = gRates.lastUs ? (now - gRates.lastUs) / 1e6 : 5.0;
    char reasons[160] = "";
    size_t at = 0;
    for (int r = 1; r < WhyCount; r++) {
        const uint64_t n = s.reasons[r].load() - gRates.reasons[r];
        if (n && at < sizeof reasons)
            at += snprintf(reasons + at, sizeof reasons - at, "%s%s:%llu", at ? "," : "", kReasonNames[r],
                           (unsigned long long)n);
        gRates.reasons[r] = s.reasons[r].load();
    }
    raw::Device::Stats &k = gRM->raw->stats;
    const double per = c ? double(c) : 1.0;
    // diagnostics (r10-rawsweep): per predicate, the command buffers that hit it / those it alone sent to m2v, the ten most hit
    char preds[256] = "";
    {
        uint64_t cbs[PredCount], only[PredCount];
        for (int i = 0; i < PredCount; i++) {
            const uint64_t now = gPredCbs[i].load(std::memory_order_relaxed),
                           o = gPredOnly[i].load(std::memory_order_relaxed);
            cbs[i] = now - gPredRates[i];
            only[i] = o - gRates.predOnly[i];
            gPredRates[i] = now;
            gRates.predOnly[i] = o;
        }
        size_t pat = 0;
        for (int shown = 0; shown < 10; shown++) {
            int best = 0;
            for (int i = 1; i < PredCount; i++)
                if (cbs[i] > cbs[best]) best = i;
            if (!best || !cbs[best] || pat >= sizeof preds) break;
            pat += snprintf(preds + pat, sizeof preds - pat, "%s%s:%llu/%llu", pat ? "," : "", kPredNames[best],
                            (unsigned long long)cbs[best], (unsigned long long)only[best]);
            cbs[best] = 0;
        }
    }
    const uint64_t aborts = s.aborted.load() - gRates.aborted, m2vs = f ? f : 1;
    char diag[512];
    snprintf(diag, sizeof diag,
             " abortenc=%.0fus/a replay=%.0fus/m rclean=%.0fKB/m rt128=%llu(pitch %u va&255 %u) why=%s",
             (s.abortUs.load() - gRates.abortUs) / double(aborts ? aborts : 1),
             (s.replayUs.load() - gRates.replayUs) / double(m2vs),
             (s.replayClean.load() - gRates.replayClean) / 1024.0 / double(m2vs),
             (unsigned long long)(k.rtUnaligned.load() - gRates.rtUnaligned), k.rtUnalignedPitch.load(),
             k.rtUnalignedVa.load(), *preds ? preds : "-");
    gRates.abortUs = s.abortUs.load();
    gRates.replayUs = s.replayUs.load();
    gRates.replayClean = s.replayClean.load();
    gRates.rtUnaligned = k.rtUnaligned.load();
    const int n = snprintf(
        out, size,
        "raw %.0fs c=%llu(%.1f/s) m2v=%llu%s%s%s sw=%llu cpu=%llu acq=%llu enc=%.0fus/c d=%.1f p=%.1f b=%.2f "
        "defer=%llu abort=%llu rw=%llu pool=%lluMB/%llu grow=%llu imp=%llu/%lluMB clean=%.1fKB/c ev=%llu/%llu%s",
        secs, (unsigned long long)c, c / secs, (unsigned long long)f, *reasons ? "(" : "", reasons, *reasons ? ")" : "",
        (unsigned long long)(s.switches.load() - gRates.switches),
        (unsigned long long)(s.cpuOrdered.load() - gRates.cpuOrdered),
        (unsigned long long)(s.acquires.load() - gRates.acquires), (s.encodeUs.load() - gRates.encodeUs) / per,
        (s.draws.load() - gRates.draws) / per, (s.passes.load() - gRates.passes) / per,
        (s.copies.load() - gRates.copies) / per, (unsigned long long)(s.deferred.load() - gRates.deferred),
        (unsigned long long)(s.aborted.load() - gRates.aborted),
        (unsigned long long)(k.ringWaits.load() - gRates.ringWaits), (unsigned long long)(k.poolBytes.load() >> 20),
        (unsigned long long)k.poolChunks.load(), (unsigned long long)(k.poolStalls.load() - gRates.poolStalls),
        (unsigned long long)k.imports.load(), (unsigned long long)(k.importBytes.load() >> 20),
        k.cleanBytes.exchange(0) / 1024.0 / per, (unsigned long long)(s.eventsReleased.load() - gRates.eventsReleased),
        (unsigned long long)(s.heldReturns.load() - gRates.heldReturns), diag);
    gRates.raw = raw;
    gRates.m2v = m2v;
    gRates.encodeUs = s.encodeUs.load();
    gRates.draws = s.draws.load();
    gRates.passes = s.passes.load();
    gRates.copies = s.copies.load();
    gRates.deferred = s.deferred.load();
    gRates.aborted = s.aborted.load();
    gRates.switches = s.switches.load();
    gRates.cpuOrdered = s.cpuOrdered.load();
    gRates.acquires = s.acquires.load();
    gRates.poolStalls = k.poolStalls.load();
    gRates.ringWaits = k.ringWaits.load();
    gRates.eventsReleased = s.eventsReleased.load();
    gRates.heldReturns = s.heldReturns.load();
    gRates.lastUs = now;
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

// raw_metal_prepare's thread and what it made (taken once, by raw_metal_device_open)
static struct {
    std::mutex lock;
    std::thread thread;
    std::string path, err;
    uint64_t stack = 0;
    raw::Device *dev = nullptr;
    bool started = false;
} gPrepared;

static raw::Device *openDevice(const char *packPath, uint64_t stack, std::string &err)
{
    raw::Device *dev = new raw::Device;
    if (!dev->open(packPath, stack, err)) {
        delete dev;  // its connection closes: the kernel frees what it held
        return nullptr;
    }
    dev->warm();
    return dev;
}

void raw_metal_prepare(const char *packPath, uint64_t stack)
{
    std::lock_guard<std::mutex> l(gPrepared.lock);
    if (gPrepared.started) return;
    gPrepared.started = true;
    gPrepared.path = packPath;
    gPrepared.stack = stack;
    gPrepared.thread = std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.ioswitch.raw.open");
        std::string err;
        raw::Device *dev = openDevice(gPrepared.path.c_str(), gPrepared.stack, err);
        std::lock_guard<std::mutex> m(gPrepared.lock);
        gPrepared.dev = dev;
        gPrepared.err = err;
    });
}

id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error)
{
    static std::mutex once;
    std::lock_guard<std::mutex> l(once);
    if (gRM) return gRM->device;  // one per process
    raw::Device *dev = nullptr;
    std::string err;
    bool prepared = false;
    {
        std::unique_lock<std::mutex> m(gPrepared.lock);
        if (gPrepared.started && gPrepared.thread.joinable()) {
            m.unlock();
            gPrepared.thread.join();  // what is left of it: it began with the plugin, before the m2v device's open
            m.lock();
        }
        if (gPrepared.started && gPrepared.path == packPath && gPrepared.stack == stack) {
            prepared = true;
            dev = gPrepared.dev;
            err = gPrepared.err;
            gPrepared.dev = nullptr;
        }
    }
    if (!prepared) dev = openDevice(packPath, stack, err);
    if (!dev) {
        if (error) *error = [NSString stringWithUTF8String:err.c_str()];
        return nil;
    }
    // The device class: a subclass of the m2v device's own (an _MTLDevice conforming to MTLDevice and MTLDeviceSPI on the iPad),
    // with no instance variables of its own, and the facade's methods over the inherited ones
    Class base = fallback ? object_getClass(fallback) : [NSObject class];
    Class cls = objc_allocateClassPair(base, "RawGM20BMetalDevice", 0);
    if (!cls) {
        if (error) *error = @"the device class could not be made";
        delete dev;
        return nil;
    }
    for (Class from : {[RMDevice class]}) {
        unsigned count = 0;
        Method *methods = class_copyMethodList(from, &count);
        for (unsigned i = 0; i < count; i++)
            class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                            method_getTypeEncoding(methods[i]));
        free(methods);
    }
    objc_registerClassPair(cls);
    RawState *r = new RawState;
    r->raw = dev;
    r->fallback = fallback;
    r->device = [cls new];
    // The base class's limits table (_MTLDevice's own accessors read it; the frontend's fillLimits filled the m2v device's from its
    // answers): the same for this device. The frontend's fillLimits, which Metal's registration runs through initLimits, finds no
    // answer of this class's own to write (they are the m2v class's), so it leaves the copy as it is.
    static const SEL limits = NSSelectorFromString(@"limits");
    if (fallback && [fallback respondsToSelector:limits] && [r->device respondsToSelector:limits]) {
        const void *from = ((const void *(*)(id, SEL))objc_msgSend)(fallback, limits);
        void *to = ((void *(*)(id, SEL))objc_msgSend)(r->device, limits);
        if (from && to && from != to) memcpy(to, from, 0x100);  // kLimitFields' extent (NativeBridgeMetalFrontend.mm)
        // and this device's own answers (RMDevice) where the table holds them (kLimitFields' offsets)
        if (to && from != to) {
            static const struct {
                uint16_t offset;
                uint32_t value;
            } own[] = {{0x78, 256}, {0xb0, 32}};
            for (const auto &o : own) memcpy(static_cast<uint8_t *>(to) + o.offset, &o.value, 4);
        }
    }
    r->handlers = dispatch_queue_create(
        "com.ioswitch.raw.handlers",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    r->rawSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->m2vSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:r->handlers];
    r->gpuOrder = dev->canOrder();
    if (!r->gpuOrder && fallback)
        ::once(@"CPU-ordered: the kernel has no Order (TegraGM20BInfoOrder): raw and m2v command buffers wait for each "
               @"other's "
               @"work to complete");
    gRM = r;
    // both threads at the render thread's class: what they hold up is the frame
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.ioswitch.raw.submit");
        submitLoop();
    }).detach();
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.ioswitch.raw.completion");
        completionLoop();
    }).detach();
    return (id<MTLDevice>)r->device;
}
