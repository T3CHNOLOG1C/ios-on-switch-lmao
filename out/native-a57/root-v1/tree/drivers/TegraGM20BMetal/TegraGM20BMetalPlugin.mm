// SPDX-License-Identifier: MIT
// TegraGM20B Metal plugin: Metal on the Tegra X1 GPU (GM20B) for iPadOS 17.7.11, reached through the original discovery:
// IOServiceMatching("IOAcceleratorES") -> AGXAccelerator (drivers/TegraGM20B, published at tegra-gpu=4) -> MetalPluginName
// TegraGM20BMetal -> /System/Library/Extensions/TegraGM20BMetal.bundle -> MetalPluginClassName TegraGM20BMetalDevice ->
// -initWithAcceleratorPort:. That returns the native Metal frontend's device (drivers/NativeMetal/NativeBridgeMetalFrontend.mm:
// the metal2vulkan runtime) over Mesa NVK (libvulkan_nouveau.dylib beside this executable, nvkmd_darwin backend), which opens
// the accelerator's AGXDeviceUserClient (scratchpad/gpu/CONTRACT.md).
//
// Adapted from UniXNU-sheng's drivers/NativeMetal/NativeAdrenoMetalPlugin.mm (published mode): the same device open, cache
// directory search and render-server preferences, without the Adreno user test, the marker stream, the scanout hand-off
// (CA's frames reach the panel through IOMobileFramebuffer and NativeLegacyDisplay as they do today), the benchmark,
// telemetry and cache-export threads, and the direct writes into QuartzCore's state.
//
// Only the processes the accelerator's NativeSandboxGrantProcess admits get a device: the render server (backboardd), the
// screenshot service (ScreenshotServicesService: PencilKit aborts its thumbnail without one) and
// the icon service (iconservicesagent: CoreUI draws every graphic icon, Settings' rows among them, through Core Image, whose
// -[CIContext initWithOptions:] in 21H461 has Metal or GLES and no CPU renderer; without Metal it makes a GLES context that does
// not exist and crashes at once, and launchd's respawns hang Settings), or every process with boot-arg tegra-gpu-all-clients=1
// (the kernel's grant must then admit them too). Every other process gets nil from here and keeps rendering on the CPU, as does
// one whose device does not open (the kernel refuses it, the GPU pool is full). Every process but the render server compiles the
// Metal source and stitched libraries Core Image makes (NMB_SYSTEM_COMPILER, the frontend's in-process MTLCompiler).
//
// Knobs: an environment variable, else the boot-arg named beside it (kern.bootargs; a sandbox that refuses that read leaves the
// default), which is then also set as the variable for the frontend and the runtime:
//   TEGRA_GPU_ALL_CLIENTS   tegra-gpu-all-clients=0   only the processes named above get a device (default: every process)
//   TEGRA_M2V_TELEMETRY     tegra-m2v-telemetry=0     no 5 s rates line (default 1)
//   TEGRA_M2V_CACHE_DIR     (environment only)        the shader cache directory instead of the search below; "memory" for none
//   NMB_MEMO_MB             tegra-m2v-memo-mb=<MiB>   the runtime's memo of compiled pipelines (default 32 outside backboardd,
//                                                     the runtime's 256 in it)
//   M2V_BLOCK_MIB           tegra-m2v-block-mib=<MiB> the largest pooled memory block (default 4 outside backboardd, 16 in it:
//                                                     each process keeps one in three pools, mapped whole and charged to it;
//                                                     each block is one BoAlloc on the thread that ran out of room)
//   NMB_CACHE_MB            tegra-m2v-cache-mb=<MiB>  the shader cache directory's cap (default 256; the runtime's is 25 GiB)
//   NMB_ANSWER_CACHE        tegra-m2v-answer-cache=0  ask the runtime every device question again (default: keep each answer)
//   NMB_REFUSE_REBINDS      tegra-m2v-refuse-rebinds=0  encoders log their calls for rebinds again (default: nothing rebinds)
//   NMB_PADDED_ALIAS        tegra-m2v-padded-alias=1  textures over padded rows rendered in place (default off: untried on NVK)
//   M2V_YCBCR_MEMO          tegra-m2v-ycbcr-memo=<N>  skip unchanged two-plane YCbCr conversions (N sampled chunks; default off)
//   M2V_YCBCR_FILTER        tegra-m2v-ycbcr-filter=device  the device's filter for converted YCbCr (default: Metal's, exact)
//   M2V_ZERO_FRESH          tegra-m2v-zero-fresh=1    zero fresh memory and upload staging on the CPU again (default off)
//   TEGRA_M2V_SEED          tegra-m2v-seed=0          no shared shader cache seed (default 1)
//   NMB_SYSTEM_COMPILER     (environment only)        Metal source and stitched libraries compiled in-process (default 1 outside
//                                                     backboardd, 0 in it)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

@interface _MTLDevice : NSObject
@end
@interface TegraGM20BMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

namespace {
os_log_t logger() {
    static os_log_t log = os_log_create("com.ioswitch.TegraGM20BMetal", "plugin");
    return log;
}

// kern.bootargs, read once.
const char *bootArgs() {
    static char args[2048];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        size_t size = sizeof args - 1;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) != 0) size = 0;
        args[std::min(size, sizeof args - 1)] = 0;
    });
    return args;
}

// Environment `env`, else boot-arg `arg` ("arg=value"; a bare "arg" is 1), then also set as `env`. Null when neither is given.
const char *knob(const char *env, const char *arg) {
    if (const char *value = getenv(env)) return value;
    const size_t length = strlen(arg);
    for (const char *at = bootArgs() + strspn(bootArgs(), " \t"); *at; at += strspn(at, " \t")) {
        const size_t word = strcspn(at, " \t");
        if (word >= length && !strncmp(at, arg, length) && (word == length || at[length] == '=')) {
            char value[256];
            if (word == length) strlcpy(value, "1", sizeof value);
            else snprintf(value, sizeof value, "%.*s", int(word - length - 1), at + length + 1);
            setenv(env, value, 0);
            return getenv(env);
        }
        at += word;
    }
    return nullptr;
}
bool knobOn(const char *env, const char *arg, bool otherwise) {
    const char *value = knob(env, arg);
    return value ? strcmp(value, "0") != 0 : otherwise;
}

// <bundle>/libvulkan_nouveau.dylib, beside this executable (<bundle>/TegraGM20BMetal).
bool libraryPath(char *path, size_t size) {
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&libraryPath), &info) || !info.dli_fname) return false;
    snprintf(path, size, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_nouveau.dylib");
    return access(path, R_OK) == 0;
}

// `path` and its missing parents, owner-only.
void makeDirectories(const char *path) {
    char partial[1024];
    strlcpy(partial, path, sizeof partial);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        struct stat st;
        if (stat(partial, &st) != 0) mkdir(partial, 0700);
        *slash = '/';
    }
    mkdir(partial, 0700);
}

// Whether this process can write in `dir`, made if missing: a probe file is created there and removed. access(2) would only ask the
// file's mode; the sandbox decides at the create.
bool writable(const char *dir) {
    makeDirectories(dir);
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/.probe.%d", dir, getpid());
    const int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    const bool wrote = write(fd, "probe", 5) == 5;
    close(fd);
    unlink(probe);
    return wrote;
}

// <process system container>/Library/Caches/m2v. The container survives reboots and the process's sandbox writes it once
// container_system_path_for_identifier (libsystem_containermanager: identifier, error out; a malloc'd path) has consumed its
// extension; backboardd's StartWindowServer makes the same lookup for Core Animation's shader cache before Metal opens. The
// identifier is the main bundle's, else com.apple.<process name> (backboardd's and iconservicesagent's own).
bool systemContainerCache(char *out, size_t size) {
    static auto lookup = (char *(*)(const char *, uint64_t *))dlsym(RTLD_DEFAULT, "container_system_path_for_identifier");
    if (!lookup) return false;
    char identifier[256];
    CFStringRef bundle = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!bundle || !CFStringGetCString(bundle, identifier, sizeof identifier, kCFStringEncodingUTF8))
        snprintf(identifier, sizeof identifier, "com.apple.%s", getprogname());
    uint64_t error = 0;
    char *container = lookup(identifier, &error);
    if (!container) return false;
    snprintf(out, size, "%s/Library/Caches/m2v", container);
    free(container);
    return true;
}

// The shader cache directories to try, most persistent first; each is this process's alone (m2v makes its files owner-only):
//  - the user cache directory when it is inside a data container (an app's own Library/Caches);
//  - the process's system container (above): backboardd's;
//  - the user cache directory elsewhere (a daemon with a home);
//  - /private/var/tmp/m2v-<process> (iconservicesagent, whose home is /var/empty), then the user temp directory.
unsigned cacheCandidates(char candidates[][512], unsigned most) {
    unsigned count = 0;
    auto add = [&](const char *format, const char *a, const char *b) {
        if (count < most) snprintf(candidates[count++], 512, format, a, b);
    };
    char user[512] = {}, temp[512] = {}, container[512] = {};
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, user, sizeof user)) user[0] = 0;
    const bool appContainer = strstr(user, "/Containers/Data/") != nullptr;
    if (user[0] && appContainer) add("%s%s", user, "m2v");
    if (!appContainer && systemContainerCache(container, sizeof container)) add("%s%s", container, "");
    if (user[0] && !appContainer) add("%s%s", user, "m2v");
    add("%s%s", "/private/var/tmp/m2v-", getprogname());
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp)) add("%s%s", temp, "m2v");
    return count;
}

// The shader cache seed shipped beside the bundle, <Extensions>/TegraGM20BMetal.m2v-seed/shared (build-tegra-gm20b-metal.py: the
// device journals' function specializations and render pipeline states made on the host with this build's m2v and the NVK twin,
// byte for byte what the device would write), named to the runtime as METAL2VULKAN_SHADER_CACHE_SEED: every process's runtime then
// reads a name its own cache directory misses from the seed, read only and in place (nothing is copied, written or scanned there),
// and writes what it compiles to its own directory as before. A seed of another build is never hit: its translations sit under
// another build's name and its driver records name another driver. The seed's SEED line (what the build made) into `name`; false
// when there is no seed this process can read, or the knob is off.
bool sharedSeed(char *name, size_t size) {
    if (!knobOn("TEGRA_M2V_SEED", "tegra-m2v-seed", true)) return false;
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&sharedSeed), &info) || !info.dli_fname) return false;
    char seed[1024], path[1100];
    strlcpy(seed, info.dli_fname, sizeof seed);
    for (int i = 0; i < 2; ++i)   // <Extensions>/TegraGM20BMetal.bundle/TegraGM20BMetal -> <Extensions>
        if (char *slash = strrchr(seed, '/')) *slash = 0;
    strlcat(seed, "/TegraGM20BMetal.m2v-seed/shared", sizeof seed);
    snprintf(path, sizeof path, "%s/SEED", seed);
    FILE *file = fopen(path, "r");   // also asks the sandbox once: a process that may not read the seed never names it
    if (!file) return false;
    const bool named = fgets(name, int(size), file) != nullptr;
    fclose(file);
    if (!named) return false;
    name[strcspn(name, "\n")] = 0;
    setenv("METAL2VULKAN_SHADER_CACHE_SEED", seed, 0);   // read by the runtime when the device opens
    return true;
}

// The frontend's rates line, every 5 s while the device works (it says nothing when idle).
void startRates() {
    if (!knobOn("TEGRA_M2V_TELEMETRY", "tegra-m2v-telemetry", true)) return;
    static dispatch_source_t timer;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(timer, ^{
        // 768: the rates line passed 512 with r6-lows' frame distributions (encp= cgap= swait= gpuq= psomax=).
        char line[768];
        if (native_bridge_metal_rates(line, sizeof line)) os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", getprogname(), line);
    });
    dispatch_resume(timer);
}

// A Metal selector the frontend does not implement (the frontend's own report, made before it raises).
void missing(const char *cls, const char *selector) {
    os_log_error(logger(), "TegraGM20BMetal: unimplemented -[%{public}s %{public}s]", cls, selector);
}

// The frontend raises on a Metal selector it does not implement (NBObject / device -doesNotRecognizeSelector:), and an
// exception in the render server restarts it into the same exception: no UI at all. Instead, a selector that some MTL*
// protocol declares is answered zero/nil/NO with one log line, as an absent optional feature would be; respondsToSelector:
// still says NO, so optional paths take their unsupported branch. A selector no Metal protocol declares raises as before.
const char *metalTypes(SEL selector) {
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
NSMethodSignature *forwardSignature(id self, SEL _cmd, SEL selector) {
    // NSObject's own answer (the methods the object really has) first; the same IMP serves NBObject's subclasses.
    static auto base = (NSMethodSignature * (*)(id, SEL, SEL))
        class_getMethodImplementation([NSObject class], @selector(methodSignatureForSelector:));
    if (NSMethodSignature *signature = base(self, _cmd, selector)) return signature;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}
void forwardZero(id self, SEL, NSInvocation *invocation) {
    static NSMutableSet<NSString *> *reported = [NSMutableSet set];   // one line per class and selector, not per frame
    NSString *key = [NSString stringWithFormat:@"%s %s", class_getName(object_getClass(self)), sel_getName(invocation.selector)];
    @synchronized(reported) {
        if (![reported containsObject:key]) {
            [reported addObject:key];
            os_log_error(logger(), "TegraGM20BMetal: unimplemented -[%{public}@] answered zero", key);
        }
    }
    const NSUInteger length = invocation.methodSignature.methodReturnLength;
    if (!length) return;
    void *zero = calloc(1, length);
    [invocation setReturnValue:zero];
    free(zero);
}
void answerUnimplemented(Class cls) {
    if (!cls) return;
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)forwardSignature, "@@::");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)forwardZero, "v@:@");
}

id openDevice(unsigned port) {
    static id device;
    static bool tried;
    @synchronized([TegraGM20BMetalDevice class]) {
        if (tried) return device;
        tried = true;
        const bool renderServer = strcmp(getprogname(), "backboardd") == 0;
        // ScreenshotServicesService: PencilKit in the screenshot thumbnail aborts the process without a device (TegraGM20B.cpp).
        if (!renderServer && strcmp(getprogname(), "iconservicesagent") != 0 &&
            strcmp(getprogname(), "ScreenshotServicesService") != 0 &&
            !knobOn("TEGRA_GPU_ALL_CLIENTS", "tegra-gpu-all-clients", true))
            return nil;
        if (!knob("NMB_MEMO_MB", "tegra-m2v-memo-mb") && !renderServer) setenv("NMB_MEMO_MB", "32", 0);   // read by the bridge
        // Read by the runtime. In the render server too: its pools double their blocks, and each block is one BoAlloc on the
        // thread whose resource creation found no room (Core Animation's render thread). At the kernel's ~1.5 ms per MiB
        // for cached memory a 64 MiB block stalled it ~100 ms (r5c: 66 MB of BoAllocs, 103 ms, in the 5 s Settings opened);
        // 16 MiB blocks bound that to a quarter and still pool any 720p texture, half a block being the dedicated limit.
        if (!knob("M2V_BLOCK_MIB", "tegra-m2v-block-mib")) setenv("M2V_BLOCK_MIB", renderServer ? "16" : "4", 0);
        if (!renderServer) setenv("NMB_SYSTEM_COMPILER", "1", 0);   // read by the frontend: Core Image's kernels are compiled here
        knob("NMB_CACHE_MB", "tegra-m2v-cache-mb");
        knob("NMB_ANSWER_CACHE", "tegra-m2v-answer-cache");
        knob("NMB_REFUSE_REBINDS", "tegra-m2v-refuse-rebinds");
        knob("NMB_PADDED_ALIAS", "tegra-m2v-padded-alias");
        knob("M2V_YCBCR_MEMO", "tegra-m2v-ycbcr-memo");
        knob("M2V_YCBCR_FILTER", "tegra-m2v-ycbcr-filter");
        if (const char *zero = knob("M2V_ZERO_FRESH", "tegra-m2v-zero-fresh"); zero && !strcmp(zero, "0"))
            unsetenv("M2V_ZERO_FRESH");   // the runtime reads any value as on
        char loader[1024];
        if (!libraryPath(loader, sizeof loader)) {
            os_log_error(logger(), "TegraGM20BMetal: no device: libvulkan_nouveau.dylib missing beside the plugin");
            return nil;
        }
        uint64_t registryID = 0;
        if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
            if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID")) entryID(port, &registryID);
        }
        // NVK opens the first service of this class (nvkmd_darwin.c darwin_open): ours, never the IOAcceleratorES stub
        // (drivers/IOSwitchAccelStub) that only ends QuartzCore's accelerator poll.
        setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
        // The render server compresses its framebuffer and surfaces in an Apple GPU layout unless told not to; this GPU and the
        // display driver take plain rows. The kernel also writes these as backboardd's preferences (read before this code runs).
        if (renderServer) {
            setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
            CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
            CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        }
        // The runtime keeps its shader cache across reboots where the directory lasts: the first candidate (cacheCandidates)
        // that takes a write, or TEGRA_M2V_CACHE_DIR. A device that does not open for another reason than its cache is not tried
        // again with the next one. With none writable the cache is the runtime's memory alone (a bridge without that mode
        // refuses, and the process gets no device as before).
        char candidates[6][512] = {};
        unsigned count = 0;
        const char *chosen = getenv("TEGRA_M2V_CACHE_DIR");
        if (chosen && strcmp(chosen, "memory")) strlcpy(candidates[count++], chosen, sizeof candidates[0]);
        else if (!chosen) count = cacheCandidates(candidates, 6);
        const char *cache = nullptr;
        char seedName[128] = {};
        const bool seeded = sharedSeed(seedName, sizeof seedName);
        for (unsigned i = 0; i < count && !device; ++i) {
            if (!writable(candidates[i])) continue;
            cache = candidates[i];
            @try {
                device = native_bridge_metal_open(loader, cache, registryID, missing);
            } @catch (NSException *exception) {
                device = nil;
            }
            if (!device && !strstr(nmb_last_error(), "ShaderCache")) break;
        }
        if (!device && (!cache || strstr(nmb_last_error(), "ShaderCache"))) {
            cache = nullptr;
            @try {
                device = native_bridge_metal_open(loader, nullptr, registryID, missing);
            } @catch (NSException *exception) {
                device = nil;
            }
        }
        if (!device) {
            os_log_error(logger(), "TegraGM20BMetal: no device in %{public}s: %{public}s", getprogname(), nmb_last_error());
            return nil;
        }
        answerUnimplemented(NSClassFromString(@"NBObject"));   // every frontend object but the device
        answerUnimplemented(object_getClass(device));
        // seed: "shared (<its SEED line>)" (the runtime reads the shared seed beneath the cache), "none" (no readable seed, or off)
        os_log(logger(), "TegraGM20BMetal: device %{public}s ready in %{public}s, cache %{public}s, seed %{public}s%{public}s%{public}s",
               [[(id<MTLDevice>)device name] UTF8String], getprogname(), cache ? cache : "memory only", seeded ? "shared (" : "none",
               seeded ? seedName : "", seeded ? ")" : "");
        startRates();
        return device;
    }
}
}  // namespace

@implementation TegraGM20BMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self = [super init];
    if (!self || !port) return nil;
    return openDevice(port);
}
@end
