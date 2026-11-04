// SPDX-License-Identifier: MIT
// The raw GM20B path as Core Animation's render server's Metal device (backboardd only; TegraGM20BMetalPlugin.mm decides).
//
// Every Metal object it hands out answers on the raw path (raw_gpu.h: direct MAXWELL_B methods, one Exec per command buffer) and
// has an m2v twin over the same memory, made when first needed (the m2v device it wraps: NativeBridgeMetalFrontend.mm). A command
// buffer is recorded as it is encoded; at commit it is encoded on the raw path, or, when anything in it is something the raw path
// does not do exactly as m2v (a state not in the pack, a format or call it lacks, compute, mipmaps...), replayed whole on m2v.
// The two drivers' command buffers run in commit order: m2v's waits for the raw work before it, raw's for the m2v work before it.
// Everything a device is asked that the raw path has no own answer for goes to the m2v device (its answers stay today's).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#ifdef __cplusplus
extern "C" {
#endif
// A raw device over the pack at `packPath` (its stack key must be `stack`; 0: any, for host tests), wrapping the m2v device
// `fallback` (nil on a host benchmark: nothing can fall back then), or nil (the reason in *error).
id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error);
// The pack key of a render pipeline descriptor whose functions are this facade's (the frontend journal's R-line fields but the label).
NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *descriptor);
// One rates line since the previous call (0 when the device did nothing): command buffers on each path and why, encode time,
// draws, passes, copies, waits, memory. The plugin logs it every 5 s with the m2v line (TEGRA_M2V_TELEMETRY).
size_t raw_metal_rates(char *out, size_t size);
#ifdef __cplusplus
}
#endif
