// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDEC user client ABI, v1 (kernel: TegraNVDECUserClient.cpp; client: SwitchVideoDecoders/AVD).
//
// The service is "TegraNVDEC" (IOServiceMatching), published when switch-nvdec=3. Its user client's class is named
// AppleD5500UserClient: that is the name iPadOS's sandbox profiles already admit wherever iPad7,1 decodes video (its
// VXD393 decoder's client), so VideoToolbox can open it from the processes that decode. Open type TEGRA_NVDEC_OPEN_TYPE.
//
// Calls: IOConnectCallStructMethod on any selector the sandbox admits, input {TEGRA_NVDEC_TUNNEL_MAGIC, method,
// record...}, output the method's record. Method TEGRA_NVDEC_TUNNEL_PROBE only proves a selector is admitted.
//
// Model (Linux tegra-drm channel submit / L4T nvhost submit, reduced to what a decoder needs):
//   * No SMMU: NVDEC reads and writes physical memory. Buffers come from one contiguous arena below 4 GiB reserved at
//     boot; user space maps them (IOConnectMapMemory64 with type = handle, cacheable) and never sees an address. Every
//     address method is a relocation (handle + offset) the kernel resolves and bounds-checks.
//   * The kernel builds the host1x stream itself: only whitelisted NVDEC methods, written through the engine's THI
//     METHOD0/METHOD1 pair, followed by an OP_DONE syncpoint increment. The picture setup travels inline and is copied
//     into kernel-only memory after its sizes are checked against the buffers it makes NVDEC write.
//   * Cache maintenance is explicit: Submit cleans the ranges the CPU wrote (bitstream, slice offsets), Wait
//     invalidates the ranges the CPU will read (status, the decoded surface). Surfaces are never written by the CPU.
//   * Jobs run in submission order; a fence is a syncpoint threshold. Wait sleeps on the syncpoint interrupt.
#ifndef TEGRA_NVDEC_ABI_H
#define TEGRA_NVDEC_ABI_H
#include <stdint.h>

#define TEGRA_NVDEC_ABI_VERSION 1
#define TEGRA_NVDEC_OPEN_TYPE 0x4e44            // 'ND'
#define TEGRA_NVDEC_TUNNEL_MAGIC 0x4e44544eu    // 'NDTN'
#define TEGRA_NVDEC_TUNNEL_PROBE 0xffffffffu

enum TegraNVDECMethod {
  TegraNVDECMethodGetInfo = 1,      // -> TegraNVDECInfo
  TegraNVDECMethodBufferAlloc = 2,  // TegraNVDECBufferAlloc -> TegraNVDECBufferReply
  TegraNVDECMethodBufferFree = 3,   // TegraNVDECBuffer -> (waits for the last job, revokes the client's mappings)
  TegraNVDECMethodSubmit = 4,       // TegraNVDECSubmit -> TegraNVDECFence
  TegraNVDECMethodWait = 5,         // TegraNVDECWait ->
};

enum { TEGRA_NVDEC_CODEC_H264 = 3 };    // NVDEC application id (clc5b0.h NVC5B0_SET_APPLICATION_ID_ID_H264)

typedef struct {
  uint32_t abi_version, nvdec_version;  // nvdec_version 0x20: NVDEC 2.0 (T210)
  uint32_t codecs;                      // bit n: TEGRA_NVDEC_CODEC_* n is accepted by Submit
  uint32_t max_width, max_height;
  uint32_t reserved;
  uint64_t arena_bytes, arena_free;
} TegraNVDECInfo;

typedef struct { uint64_t size; } TegraNVDECBufferAlloc;
typedef struct { uint32_t handle, reserved; uint64_t size; } TegraNVDECBufferReply;   // size rounded to 64 KiB
typedef struct { uint32_t handle, reserved; } TegraNVDECBuffer;
typedef struct { uint32_t handle, offset, length, reserved; } TegraNVDECRange;

// One NVDEC method. kind 0: `value` is written as is. kind 1: relocation, the method gets
// (physical address of buffer `value` + `offset`) >> 8, so `offset` must be 256-byte aligned.
typedef struct {
  uint16_t method;                      // byte offset in the NVDEC class (clc5b0.h)
  uint16_t kind;
  uint32_t value, offset;
} TegraNVDECEntry;

enum { TEGRA_NVDEC_ENTRY_VALUE = 0, TEGRA_NVDEC_ENTRY_RELOC = 1 };

#define TEGRA_NVDEC_MAX_ENTRIES 64
#define TEGRA_NVDEC_MAX_RANGES 4
#define TEGRA_NVDEC_MAX_SETUP 1024
typedef struct {
  uint32_t codec;                       // TEGRA_NVDEC_CODEC_*
  uint32_t entry_count, clean_count;
  uint32_t setup_bytes;                 // sizeof(nvdec_h264_pic_s) for H.264
  TegraNVDECEntry entries[TEGRA_NVDEC_MAX_ENTRIES];   // no SET_DRV_PIC_SETUP_OFFSET: the kernel adds it
  TegraNVDECRange clean[TEGRA_NVDEC_MAX_RANGES];
  uint8_t setup[TEGRA_NVDEC_MAX_SETUP];
} TegraNVDECSubmit;

typedef struct { uint32_t fence, reserved; } TegraNVDECFence;

typedef struct {
  uint32_t fence, timeout_us;
  uint32_t invalidate_count, reserved;
  TegraNVDECRange invalidate[TEGRA_NVDEC_MAX_RANGES];
} TegraNVDECWait;

#ifdef __cplusplus
static_assert(sizeof(TegraNVDECEntry) == 12, "entry");
static_assert(sizeof(TegraNVDECSubmit) == 16 + 12 * 64 + 16 * 4 + 1024, "submit");
static_assert(sizeof(TegraNVDECSubmit) + 8 <= 4096, "submit travels inline");
#endif
#endif
