"""SwitchAudio's S32 link (r17-audio): the APE and codec register words for S32_LE samples from memory to the RT5639,
and the plug-in's float32 <-> S32 conversions, as shipped (drivers/SwitchAudio/SwitchAudioFormat.h,
hal/SwitchAudioConvert.h through tests/switch_audio_format_harness.c).

Register words are re-derived here from the Linux sources' formulas for that stream, independently of the header:
NVIDIA L4T sound/soc/tegra-alt (tegra210_xbar_set_cif, tegra_admaif_hw_params and tegra210_i2s_hw_params for
SNDRV_PCM_FORMAT_S32_LE) and mainline sound/soc/codecs/rt5640.c (rt5640_set_dai_fmt for CBS_CFS/I2S/NB_NF,
rt5640_hw_params and rl6231_get_clk_info for 24-bit samples, 2 channels, MCLK 256 fs). The driver's own source must
program exactly those words (no hand-written duplicates), and its I2S clock must still be 47999.878 Hz on PLLA.

Conversions: full scale 2^31 with saturation (+1.0 -> INT32_MAX, -1.0 -> INT32_MIN, NaN -> 0), two IO contexts sum
with saturation, ring wrap-around in two runs, capture back to float exactly for 24-bit samples; the NEON build (as on
the device) and the scalar fallback agree bit for bit."""
import ctypes
import math
import random
import re
import struct
import subprocess
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIO = ROOT / 'drivers/SwitchAudio'
HARNESS = ROOT / 'tests/switch_audio_format_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(AUDIO),
         '-I', str(AUDIO / 'hal')]

INT32_MAX, INT32_MIN = 2**31 - 1, -2**31
RATE = 48000
CHANNELS = 2

# tegra210_xbar_alt.h TEGRA210_AUDIOCIF_BITS_*, tegra210_i2s_alt.h TEGRA210_I2S_BITS_*.
BITS = {8: 1, 16: 3, 24: 5, 32: 7}


def l4t_cif(threshold=0, audio_channels=1, client_channels=1, audio_bits=0, client_bits=0, expand=0, stereo_conv=0,
            replicate=0, truncate=0, mono_conv=0):
    """tegra210_xbar_utils_alt.c tegra210_xbar_set_cif (written under mask 0x3fffffff)."""
    value = (threshold << 24 | (audio_channels - 1) << 20 | (client_channels - 1) << 16 | audio_bits << 12 |
             client_bits << 8 | expand << 6 | stereo_conv << 4 | replicate << 3 | truncate << 1 | mono_conv)
    return value & 0x3fffffff


def l4t_admaif(fmt, channels):
    """tegra_admaif_hw_params: the channel's CIF word with tegra_admaif_set_pack_mode's bits (PACK8 31, PACK16 30)."""
    bits, valid = {'S8': (8, 8), 'S16_LE': (16, 16), 'S24_LE': (24, 32), 'S32_LE': (32, 32)}[fmt]
    pack = (1 << 31) if valid == 8 else (1 << 30) if valid == 16 else 0
    return pack | l4t_cif(0, channels, channels, BITS[bits], BITS[bits])


def l4t_i2s(fmt, channels, srate, bclk_ratio=0):
    """tegra210_i2s_hw_params in LRCK mode: (I2S_CTRL bit size, i2s clock, I2S_TIMING)."""
    sample_size = {'S8': 8, 'S16_LE': 16, 'S24_LE': 24, 'S32_LE': 32}[fmt]
    client_channels = 2 if channels == 1 else channels
    i2sclock = srate * sample_size * client_channels
    if bclk_ratio:
        i2sclock *= bclk_ratio
    bitcnt = (i2sclock // srate - 1) >> 1
    timing = bitcnt | ((1 << 12) if i2sclock % (2 * srate) else 0)
    return BITS[sample_size], i2sclock, timing


# rt5640.h fields.
RT5640_I2S_MS_S, RT5640_I2S_BP_NOR, RT5640_I2S_DF_I2S = 1 << 15, 0, 0
RT5640_SDP_FMT_MASK = (1 << 15) | (1 << 7) | 3     # rt5640_set_dai_fmt: MS, BP, DF
RT5640_I2S_DL_MASK = 3 << 2
RT5640_I2S_DL = {16: 0, 20: 1 << 2, 24: 2 << 2, 8: 3 << 2}
RT5640_I2S_BCLK_MS1_SFT, RT5640_I2S_PD1_SFT = 15, 12
RT5640_ADDA_MASK = (1 << 15) | (7 << 12)


def rl6231_get_clk_info(sclk, rate):
    """rl6231.c: the pre-divider index with sclk == rate * 256 * pd."""
    for i, pd in enumerate((1, 2, 3, 4, 6, 8, 12, 16)):
        if sclk == (rate << 8) * pd:
            return i
    raise ValueError('no pre-divider')


def rt5640(width, channels, rate, sysclk):
    """rt5640_set_dai_fmt (CBS_CFS, I2S, NB_NF) then rt5640_hw_params for IF1: (SDP mask, SDP, ADDA mask, ADDA)."""
    if width not in RT5640_I2S_DL:
        raise ValueError('rt5640_hw_params: -EINVAL for width %d' % width)
    frame_size = width * channels                        # snd_soc_params_to_frame_size
    bclk_ms = 1 if frame_size > 32 else 0
    pre_div = rl6231_get_clk_info(sysclk, rate)
    sdp = RT5640_I2S_MS_S | RT5640_I2S_BP_NOR | RT5640_I2S_DF_I2S | RT5640_I2S_DL[width]
    adda = bclk_ms << RT5640_I2S_BCLK_MS1_SFT | pre_div << RT5640_I2S_PD1_SFT
    return RT5640_SDP_FMT_MASK | RT5640_I2S_DL_MASK, sdp, RT5640_ADDA_MASK, adda, 32 << bclk_ms


def constant(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', text)
    return int(match.group(1), 0) if match else None


def fcvtzs31(x):
    """FCVTZS #31 on a float32: x * 2^31 rounded toward zero, saturated; NaN is 0."""
    if math.isnan(x):
        return 0
    v = x * 2.0**31
    if v >= 2**31:
        return INT32_MAX
    if v <= -2**31:
        return INT32_MIN
    return int(v)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class SwitchAudioFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-audio-format-')
        cls.libs = {}
        for name, extra in (('neon', []), ('scalar', ['-U__ARM_NEON'])):
            library = Path(cls.directory.name) / ('libaudioformat-%s.dylib' % name)
            subprocess.run(['clang', *FLAGS, *extra, '-dynamiclib', '-o', str(library), str(HARNESS)], check=True)
            lib = ctypes.CDLL(str(library))
            u32, u64, i32 = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_int32
            for fn in ('h_admaif_cif', 'h_i2s_divider'):
                getattr(lib, fn).argtypes = [u32]
            lib.h_cif.argtypes = [u32] * 5
            for fn in ('h_cif', 'h_admaif_cif', 'h_i2s_rx_cif', 'h_i2s_tx_cif', 'h_i2s_ctrl', 'h_i2s_timing',
                       'h_i2s_divider', 'h_bclk_fs', 'h_codec_bits', 'h_rt5640_sdp_mask', 'h_rt5640_sdp',
                       'h_rt5640_adda_mask', 'h_rt5640_adda'):
                getattr(lib, fn).restype = u32
            lib.h_ticks_per_frame_q32.argtypes = [u64] * 7
            lib.h_ticks_per_frame_q32.restype = u64
            lib.h_rate_millihz.argtypes = [u64] * 6
            lib.h_rate_millihz.restype = u64
            lib.h_mix.argtypes = [ctypes.POINTER(i32), ctypes.POINTER(ctypes.c_float), u32, ctypes.c_float]
            lib.h_ring_mix.argtypes = [ctypes.POINTER(i32), u32, u32, u64, ctypes.POINTER(ctypes.c_float), u32,
                                       ctypes.c_float]
            lib.h_ring_mix.restype = u32
            lib.h_s32_to_float.argtypes = [i32]
            lib.h_s32_to_float.restype = ctypes.c_float
            lib.h_neon.restype = ctypes.c_int
            cls.libs[name] = lib
        cls.lib = cls.libs['neon']
        cls.driver = (AUDIO / 'SwitchAudio.cpp').read_text()
        cls.plugin = (AUDIO / 'hal/SwitchAudioPlugIn.c').read_text()
        cls.abi = (AUDIO / 'SwitchAudioAbi.h').read_text()

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    # ---- register words ------------------------------------------------------------------------------------------
    def test_admaif_words_are_l4t_s32(self):
        for channels in (1, 2):
            self.assertEqual(self.lib.h_admaif_cif(channels), l4t_admaif('S32_LE', channels))
        # The format it replaces, for the record: PACK16 with 16-bit audio and client bits.
        self.assertEqual(l4t_admaif('S16_LE', 2), (1 << 30) | 0x00113300)

    def test_i2s_words_are_l4t_s32(self):
        bit_size, i2sclock, timing = l4t_i2s('S32_LE', CHANNELS, RATE)
        self.assertEqual(self.lib.h_i2s_ctrl() & 7, bit_size)
        self.assertEqual(self.lib.h_i2s_ctrl() & ~7 & 0xffffffff, (31 << 24) | (1 << 10))  # fsync 31, master, LRCK
        self.assertEqual(self.lib.h_i2s_timing(), timing)
        self.assertEqual(i2sclock, 3072000)
        self.assertEqual(self.lib.h_bclk_fs() * RATE, i2sclock)
        # Playback CIF (AXBAR RX, XBAR -> DAP): 2/2 channels, RX FIFO threshold 3 (rx_fifo_th default); capture CIF
        # (AXBAR TX, DAP -> XBAR): stereo_to_mono channel 0, audio 1 channel, client 2.
        self.assertEqual(self.lib.h_i2s_rx_cif(), l4t_cif(3, 2, 2, BITS[32], BITS[32]))
        self.assertEqual(self.lib.h_i2s_tx_cif(), l4t_cif(0, 1, 2, BITS[32], BITS[32], stereo_conv=0))

    def test_codec_words_are_rt5640_24_bit(self):
        sdp_mask, sdp, adda_mask, adda, bclk_fs = rt5640(24, CHANNELS, RATE, 256 * RATE)
        self.assertEqual(self.lib.h_rt5640_sdp_mask(), sdp_mask)
        self.assertEqual(self.lib.h_rt5640_sdp(), sdp)
        self.assertEqual(self.lib.h_rt5640_adda_mask(), adda_mask)
        self.assertEqual(self.lib.h_rt5640_adda(), adda)
        self.assertEqual(self.lib.h_codec_bits(), 24)
        # The codec's BCLK/LRCK ratio is the Tegra's bit clock; it has no 32-bit width (S32 would be -EINVAL).
        self.assertEqual(bclk_fs, self.lib.h_bclk_fs())
        with self.assertRaises(ValueError):
            rt5640(32, CHANNELS, RATE, 256 * RATE)

    def test_i2s_clock_is_a_sixteenth_of_plla_out0(self):
        m, n, p = (constant(self.driver, k) for k in ('kPllaM', 'kPllaN', 'kPllaP'))
        sdm = constant(self.driver, 'kPllaSdm')
        self.assertEqual((m, n, p, sdm), (2, 38, 2, 0xfccc))
        sdm_signed = sdm - 0x10000 if sdm & 0x8000 else sdm
        n_eff = 8192 * n + 4096 + sdm_signed
        plla = Fraction(38400000 * n_eff, 8192 * m * p)
        self.assertEqual(plla, Fraction(737278125, 2))  # 368639062.5 Hz (clk-tegra210b01.c table entry)
        out0_x2 = constant(self.driver, 'kPllaOut0X2')
        per_fs = constant(self.driver, 'kPllaOut0PerFs')
        self.assertEqual((out0_x2, per_fs), (15, 1024))
        self.assertEqual(constant(self.driver, 'kPllaOut0Div'), None)  # derived: kPllaOut0X2 - 2 (7.1 of 7.5)
        self.assertIn('kPllaOut0Div = kPllaOut0X2 - 2', self.driver)
        plla_out0 = plla * 2 / out0_x2
        divider = self.lib.h_i2s_divider(per_fs)
        self.assertEqual(divider, 30)
        bclk = plla_out0 / (Fraction(divider, 2) + 1)  # 7.1 divider: ratio = value / 2 + 1
        fs = bclk / self.lib.h_bclk_fs()
        self.assertEqual(fs, plla / (Fraction(15, 2) * 32 * 32))  # the 16-bit link's rate: unchanged
        self.assertAlmostEqual(float(fs), 47999.878, places=3)
        self.assertEqual(self.lib.h_rate_millihz(38400000, m, p, n_eff, out0_x2, per_fs), math.floor(fs * 1000))
        for host_hz in (19200000, 24000000):
            q32 = self.lib.h_ticks_per_frame_q32(host_hz, 38400000, m, p, n_eff, out0_x2, per_fs)
            self.assertEqual(q32, math.floor(Fraction(host_hz) / fs * 2**32))
        # The driver programs the rate through these helpers.
        self.assertIn('switch_audio_ticks_per_frame_q32(hz, refHz, kPllaM, kPllaP, (uint64_t)kPllaNEff, kPllaOut0X2,',
                      self.driver)

    def test_driver_programs_the_header_words(self):
        d = self.driver
        for line in ('w32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));',
                     'w32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));',
                     'w32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());',
                     'w32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());',
                     'w32(ahub, I2S_CTRL, kI2sCtrl);', 'w32(ahub, I2S_TIMING, kI2sTiming);',
                     'w32(car, CLK_SOURCE_I2S0, kI2sDiv);',
                     'codecUpdate(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);',
                     'codecUpdate(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);'):
            self.assertIn(line, d)
        self.assertIn('kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing()', d)
        self.assertIn('kI2sDiv = switch_audio_i2s_divider(kPllaOut0PerFs)', d)
        for gone in ('kPack16', 'constexpr uint32_t cif(', 'sizeof(int16_t)', 'int16_t *'):
            self.assertNotIn(gone, d)
        self.assertEqual(len(re.findall(r'w32\(ahub, (?:ADMAIF|I2S)_[RT]X_CIF,', d)), 4)  # nothing else writes them

    def test_abi_is_s32(self):
        self.assertIn('#define SWITCH_AUDIO_VERSION       3u', self.abi)
        self.assertIn('typedef int32_t switch_audio_sample_t;', self.abi)
        self.assertIn('sizeof(switch_audio_sample_t)', self.driver)
        self.assertNotIn('int16_t', self.plugin)
        self.assertIn('switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, main, frames, gain())',
                      self.plugin)
        self.assertIn('switch_audio_s32_to_float(gIn[', self.plugin)
        # Periods stay whole ADMA buffers and the rings DMA-able: 16 KiB playback periods, 128 KiB ring.
        self.assertEqual(16384 * CHANNELS * 4, 128 * 1024)

    # ---- conversions -----------------------------------------------------------------------------------------------
    def mix(self, dst, src, gain=1.0, lib=None):
        lib = lib or self.lib
        out = (ctypes.c_int32 * len(dst))(*dst)
        values = (ctypes.c_float * len(src))(*src)
        lib.h_mix(out, values, len(src), gain)
        return list(out)

    def test_full_scale_and_saturation(self):
        src = [1.0, -1.0, 0.5, -0.5, 0.0, -0.0, 2.0, -2.0, float('nan'), float('inf'), float('-inf'), 2**-31, 0.999999]
        got = self.mix([0] * len(src), src)
        self.assertEqual(got[:8], [INT32_MAX, INT32_MIN, 2**30, -2**30, 0, 0, INT32_MAX, INT32_MIN])
        self.assertEqual(got[8:11], [0, INT32_MAX, INT32_MIN])
        self.assertEqual(got[11], 1)
        self.assertEqual(got, [fcvtzs31(f32(x)) for x in src])

    def test_contexts_sum_with_saturation(self):
        a = [0.25, 0.75, -0.75, 0.5, -1.0, 0.1]
        b = [0.25, 0.75, -0.75, -0.5, -1.0, -0.1]
        once = self.mix([0] * 6, a)
        twice = self.mix(once, b)
        expect = [max(INT32_MIN, min(INT32_MAX, fcvtzs31(f32(x)) + fcvtzs31(f32(y)))) for x, y in zip(a, b)]
        self.assertEqual(twice, expect)
        self.assertEqual(twice[:5], [2**30, INT32_MAX, INT32_MIN, 0, INT32_MIN])

    def test_gain(self):
        got = self.mix([0] * 4, [1.0, -1.0, 0.5, 0.25], gain=0.5)
        self.assertEqual(got, [2**30, -2**30, 2**29, 2**28])
        self.assertEqual(self.mix([7] * 3, [0.9, -0.9, 0.3], gain=0.0), [7, 7, 7])

    def test_every_length_neon_matches_scalar(self):
        rng = random.Random(17)
        self.assertEqual(self.libs['neon'].h_neon(), 1)
        self.assertEqual(self.libs['scalar'].h_neon(), 0)
        for n in list(range(0, 13)) + [1023, 2048]:
            src = [f32(rng.uniform(-1.3, 1.3)) for _ in range(n)]
            dst = [rng.randrange(INT32_MIN, INT32_MAX) for _ in range(n)]
            gain = f32(rng.uniform(0.0, 1.0))
            neon = self.mix(dst, src, gain, self.libs['neon'])
            scalar = self.mix(dst, src, gain, self.libs['scalar'])
            self.assertEqual(neon, scalar, 'length %d' % n)
            expect = [max(INT32_MIN, min(INT32_MAX, d + fcvtzs31(f32(s * gain)))) for d, s in zip(dst, src)]
            self.assertEqual(neon, expect, 'length %d' % n)

    def test_ring_wrap(self):
        ring_frames, channels = 16, 2
        for first in (0, 5, 12, 15, 16, 16 * 1000 + 13):
            for frames in (0, 1, 3, 4, 11, 16, 20):
                ring = (ctypes.c_int32 * (ring_frames * channels))()
                src_values = [f32((i + 1) / 64.0) * (1 if i % 2 else -1) for i in range(frames * channels)]
                src = (ctypes.c_float * max(1, len(src_values)))(*src_values)
                run = self.lib.h_ring_mix(ring, ring_frames, channels, first, src, frames, 1.0)
                total = min(frames, ring_frames)
                self.assertEqual(run, min(total, ring_frames - first % ring_frames))
                expect = [0] * (ring_frames * channels)
                for i in range(total):
                    at = (first + i) % ring_frames
                    for c in range(channels):
                        expect[at * channels + c] += fcvtzs31(src_values[i * channels + c])
                self.assertEqual(list(ring), expect, 'first %d frames %d' % (first, frames))

    def test_capture_back_to_float(self):
        self.assertEqual(self.lib.h_s32_to_float(INT32_MIN), -1.0)
        self.assertEqual(self.lib.h_s32_to_float(2**30), 0.5)
        self.assertEqual(self.lib.h_s32_to_float(0), 0.0)
        rng = random.Random(5)
        for _ in range(2000):
            v24 = rng.randrange(-2**23, 2**23)          # the codec's 24 bits, top-aligned in the slot
            sample = v24 << 8
            x = self.lib.h_s32_to_float(sample)
            self.assertEqual(x, v24 / 2.0**23)          # exact
            self.assertEqual(self.mix([0], [x])[0], sample)  # and back


if __name__ == '__main__':
    unittest.main()
