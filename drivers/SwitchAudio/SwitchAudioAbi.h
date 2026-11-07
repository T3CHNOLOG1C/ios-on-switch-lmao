/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the kernel service SwitchAudio (SwitchAudio.cpp) and the audio server plug-in iPadOS loads
 * (hal/SwitchAudioPlugIn.c). Modelled on UniXNU-sheng's package/ShengAudioAbi.h.
 *
 * The plug-in opens the service, maps three memory types and starts and stops each direction. Memory type 0 is the
 * control page, 1 the playback ring, 2 the capture ring. Rings hold 32-bit little-endian signed interleaved samples
 * (S32_LE, full scale +-2^31), ring_frames frames each; sample time t lives at frame t % ring_frames. The Tegra
 * backend hands them to the APE as they are (SwitchAudioFormat.h): the codec plays the top 24 bits of each.
 *
 * Sample time 0 is host time anchor_host, and one frame lasts ticks_per_frame_q32 / 2^32 host ticks. The null backend
 * sets both once; the Tegra I2S backend sets anchor_host again each time the hardware starts (the host time its DMA
 * started at ring frame 0) and ticks_per_frame_q32 to the frame length of the I2S clock it programmed (47999.88 Hz on
 * this board's PLLA, not 48000), so the plug-in reads anchor_host after SwitchAudioStartOutput returns. rate is the
 * nominal rate both directions run at now, one of rates[] (0 ends the list); SwitchAudioSetRate changes it, and
 * ticks_per_frame_q32 with it, only while neither direction runs. The plug-in
 * mixes playback into the ring (adds, it does not overwrite) at least lead_frames ahead of now; the service consumes
 * the ring and clears what it consumed (out_frames). Capture frames below in_frames are valid. Both positions advance
 * in steps of period_frames (480 null, 2048 Tegra), so capture is read at least period_frames + lead_frames behind
 * now.
 *
 * The Tegra backend's ADMA plays and fills the rings in place and does not snoop the CPU caches: the plug-in cleans
 * (dc cvac) every playback range it mixed before returning, and the service invalidates each capture period before
 * it moves in_frames past it. An ADMA cycle holds at most 8 periods, so a ring is exactly 8 of them. */
#ifndef SWITCH_AUDIO_ABI_H
#define SWITCH_AUDIO_ABI_H
#include <stdint.h>

#define SWITCH_AUDIO_CLASS         "SwitchAudio"
#define SWITCH_AUDIO_MAGIC         0x53574155u /* 'SWAU' */
#define SWITCH_AUDIO_VERSION       4u
#define SWITCH_AUDIO_RATE          48000u /* the rate a service starts at */
#define SWITCH_AUDIO_MAX_RATES     2u
#define SWITCH_AUDIO_OUT_CHANNELS  2u
#define SWITCH_AUDIO_IN_CHANNELS   1u
#define SWITCH_AUDIO_PERIOD_FRAMES 480u   /* 10 ms: the null backend's step */
#define SWITCH_AUDIO_RING_FRAMES   16384u /* 341 ms: 8 ADMA periods of 2048 frames */
typedef int32_t switch_audio_sample_t;    /* a ring sample: S32_LE */

enum {
    SwitchAudioMemoryControl = 0,
    SwitchAudioMemoryOutput = 1,
    SwitchAudioMemoryInput = 2
};

enum {
    SwitchAudioStartOutput = 0,
    SwitchAudioStopOutput = 1,
    SwitchAudioStartInput = 2,
    SwitchAudioStopInput = 3,
    /* One scalar: the address, in the caller, of CoreAudio's 24 MHz host clock constant (see SwitchAudio.cpp). */
    SwitchAudioSetHostClock = 4,
    /* One scalar: the nominal rate in Hz, one of rates[]. kIOReturnBusy while either direction runs. */
    SwitchAudioSetRate = 5
};

enum {
    SwitchAudioOutputRunning = 1u << 0,
    SwitchAudioInputRunning = 1u << 1
};

enum {
    SwitchAudioBackendNull = 0,
    SwitchAudioBackendTegraI2S = 1
};

typedef struct {
    uint32_t magic, version;
    volatile uint32_t rate;
    uint32_t out_channels, in_channels, period_frames, ring_frames, lead_frames;
    uint32_t backend;
    uint32_t rates[SWITCH_AUDIO_MAX_RATES];
    volatile uint32_t state;               /* SwitchAudio*Running */
    volatile uint64_t ticks_per_frame_q32; /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t anchor_host;         /* host time of sample time 0 */
    volatile uint64_t out_frames;          /* the service: playback consumed (and cleared) below this sample time */
    volatile uint64_t in_frames;           /* the service: capture valid below this sample time */
    volatile uint64_t out_written;         /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t io_cycles;           /* the plug-in: playback IO cycles (WriteMix calls) since it mapped this */
    volatile uint64_t io_frames;           /* the plug-in: frames those cycles wrote */
} SwitchAudioControl;

#endif
