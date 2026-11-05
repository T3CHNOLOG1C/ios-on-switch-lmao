// SPDX-License-Identifier: MIT
// Entry points of the native Metal frontend over the bridge (NativeBridgeMetalFrontend.mm).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
extern "C" {
// Opens the process's one device: loader exports vkGetInstanceProcAddr (Turnip), cache is a writable shader-cache directory, registryID is the
// accelerator service's registry entry. missing (may be null) is told the class and selector of every unimplemented call.
id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID,
                            void (*missing)(const char *, const char *));
// Every Metal selector `cls` (and its subclasses) lacks that an MTL* protocol declares is answered zero/nil/NO through message
// forwarding, `answered` (may be null) told each class and selector once; respondsToSelector: still answers NO. Each selector's
// protocols are looked up once: a forwarded call costs an NSInvocation. Counted in the rates line (fwd=).
void native_bridge_metal_answer_unimplemented(Class cls, void (*answered)(const char *, const char *));
// Bare AIR bitcode as a library (the bridge's fixtures; Metal itself accepts only metallibs).
id native_bridge_metal_library_with_air(const void *bytes, size_t length);
// One line for the plugin's status report: command buffers committed/failed locally, pipelines made, refusals, the last refusal's text.
size_t native_bridge_metal_status(char *out, size_t size);
// The pixel formats of the textures asked for (first eight distinct) and how many texture requests the runtime refused.
size_t native_bridge_metal_formats(char *out, size_t size);
size_t native_bridge_metal_telemetry(char *out, size_t size);
// One line of rates since the previous call (commits/s, CPU per commit, submission and waits, surface maintenance, compiles, cache
// hits); 0 when nothing was committed or compiled since. The plugin logs it every 5 s.
size_t native_bridge_metal_rates(char *out, size_t size);
// Structured cumulative artifact/driver-cache counters for offline qualification.
struct NmbShaderCacheStats;
bool native_bridge_metal_shader_cache_stats(struct NmbShaderCacheStats *out);
bool native_bridge_metal_work_stats(uint64_t out[10]);
// The same fixed CPU, memory and pipeline workload on the tablet and the host, so their times compare directly (int-145): compute, first-touch
// page faults, memset bandwidth, CommonCrypto SHA-256, then the first `pairs` vertex/fragment pairs of the given QuartzCore metallib made into
// BGRA8 render states and drawn once each (pipeline_timing_host's draw=1 order). The results become the telemetry's Y rows; also returned.
size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size);
// A texture over `buffer` (this device's) rendered and sampled in place, as an IOSurface's: the raw GM20B path's m2v twin of memory
// it shares (drivers/TegraGM20BMetal/raw/runtime/RawMetal.mm). Nil if the buffer is not this device's or the runtime refuses.
id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow);
}
#endif
