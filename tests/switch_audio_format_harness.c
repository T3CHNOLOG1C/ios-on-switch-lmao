// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for tests/test_switch_audio_format.py: the APE/codec register words of SwitchAudio's S32 link
// (drivers/SwitchAudio/SwitchAudioFormat.h) and the plug-in's float32 <-> S32 conversions
// (drivers/SwitchAudio/hal/SwitchAudioConvert.h), compiled as they ship and exported for ctypes. Built twice by the
// test: with NEON (the arm64 host, as the device) and with __ARM_NEON undefined (the scalar fallback), to compare.
#include <stdint.h>

#include "SwitchAudioConvert.h"
#include "SwitchAudioFormat.h"

uint32_t h_cif(uint32_t threshold, uint32_t audio_channels, uint32_t client_channels, uint32_t audio_bits,
               uint32_t client_bits)
{
    return switch_audio_cif(threshold, audio_channels, client_channels, audio_bits, client_bits);
}
uint32_t h_admaif_cif(uint32_t channels)
{
    return switch_audio_admaif_cif(channels);
}
uint32_t h_i2s_rx_cif(void)
{
    return switch_audio_i2s_rx_cif();
}
uint32_t h_i2s_tx_cif(void)
{
    return switch_audio_i2s_tx_cif();
}
uint32_t h_i2s_ctrl(void)
{
    return switch_audio_i2s_ctrl();
}
uint32_t h_i2s_timing(void)
{
    return switch_audio_i2s_timing();
}
uint32_t h_i2s_divider(uint32_t pllaout0_per_fs)
{
    return switch_audio_i2s_divider(pllaout0_per_fs);
}
uint32_t h_bclk_fs(void)
{
    return SWITCH_AUDIO_BCLK_FS;
}
uint32_t h_codec_bits(void)
{
    return SWITCH_AUDIO_CODEC_BITS;
}
uint32_t h_rt5640_sdp_mask(void)
{
    return SWITCH_AUDIO_RT5640_SDP_MASK;
}
uint32_t h_rt5640_sdp(void)
{
    return SWITCH_AUDIO_RT5640_SDP;
}
uint32_t h_rt5640_adda_mask(void)
{
    return SWITCH_AUDIO_RT5640_ADDA_MASK;
}
uint32_t h_rt5640_adda(void)
{
    return SWITCH_AUDIO_RT5640_ADDA;
}
uint64_t h_ticks_per_frame_q32(uint64_t host_hz, uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff,
                               uint64_t out0_x2, uint64_t pllaout0_per_fs)
{
    return switch_audio_ticks_per_frame_q32(host_hz, ref_hz, m, p, n_eff, out0_x2, pllaout0_per_fs);
}
uint64_t h_rate_millihz(uint64_t ref_hz, uint64_t m, uint64_t p, uint64_t n_eff, uint64_t out0_x2,
                        uint64_t pllaout0_per_fs)
{
    return switch_audio_rate_millihz(ref_hz, m, p, n_eff, out0_x2, pllaout0_per_fs);
}
void h_mix(int32_t *dst, const float *src, uint32_t samples, float gain)
{
    switch_audio_mix_s32(dst, src, samples, gain);
}
uint32_t h_ring_mix(int32_t *ring, uint32_t ring_frames, uint32_t channels, uint64_t first, const float *src,
                    uint32_t frames, float gain)
{
    return switch_audio_ring_mix(ring, ring_frames, channels, first, src, frames, gain);
}
float h_s32_to_float(int32_t v)
{
    return switch_audio_s32_to_float(v);
}
int h_neon(void)
{
#if defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}
