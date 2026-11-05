// SwitchAudio's S32 link re-derived from the Linux sources' formulas for that stream, independently of
// SwitchAudioFormat.h: NVIDIA L4T sound/soc/tegra-alt (tegra210_xbar_set_cif, tegra_admaif_hw_params and
// tegra210_i2s_hw_params for SNDRV_PCM_FORMAT_S32_LE) and mainline sound/soc/codecs/rt5640.c (rt5640_set_dai_fmt for
// CBS_CFS/I2S/NB_NF, rt5640_hw_params and rl6231_get_clk_info for 24-bit samples, 2 channels, MCLK 256 fs). Also the
// exact rational arithmetic the PLLA clock checks use.
#pragma once
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>

namespace switch_audio_linux {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;

// tegra210_xbar_alt.h TEGRA210_AUDIOCIF_BITS_*, tegra210_i2s_alt.h TEGRA210_I2S_BITS_*.
constexpr uint32_t bits(unsigned width)
{
    return width == 8 ? 1 : width == 16 ? 3 : width == 24 ? 5 : width == 32 ? 7 : 0;
}

struct Cif {
    uint32_t threshold = 0, audioChannels = 1, clientChannels = 1, audioBits = 0, clientBits = 0;
    uint32_t expand = 0, stereoConv = 0, replicate = 0, truncate = 0, monoConv = 0;
};

// tegra210_xbar_utils_alt.c tegra210_xbar_set_cif (written under mask 0x3fffffff).
constexpr uint32_t cif(const Cif &c)
{
    const uint32_t value = c.threshold << 24 | (c.audioChannels - 1) << 20 | (c.clientChannels - 1) << 16 |
                           c.audioBits << 12 | c.clientBits << 8 | c.expand << 6 | c.stereoConv << 4 |
                           c.replicate << 3 | c.truncate << 1 | c.monoConv;
    return value & 0x3fffffff;
}

// tegra_admaif_hw_params: the channel's CIF word with tegra_admaif_set_pack_mode's bits (PACK8 31, PACK16 30).
// `format` is "S8", "S16_LE", "S24_LE" or "S32_LE".
inline uint32_t admaif(const std::string &format, uint32_t channels)
{
    const unsigned width = format == "S8" ? 8 : format == "S16_LE" ? 16 : format == "S24_LE" ? 24 : 32;
    const unsigned valid = format == "S8" ? 8 : format == "S16_LE" ? 16 : 32;
    const uint32_t pack = valid == 8 ? 1u << 31 : valid == 16 ? 1u << 30 : 0;
    Cif c;
    c.audioChannels = c.clientChannels = channels;
    c.audioBits = c.clientBits = bits(width);
    return pack | cif(c);
}

struct I2s {
    uint32_t bitSize, i2sClock, timing;
};

// tegra210_i2s_hw_params in LRCK mode: I2S_CTRL bit size, the i2s clock, I2S_TIMING.
constexpr I2s i2s(unsigned sampleSize, uint32_t channels, uint32_t rate, uint32_t bclkRatio = 0)
{
    const uint32_t clientChannels = channels == 1 ? 2 : channels;
    uint32_t clock = rate * sampleSize * clientChannels;
    if (bclkRatio) clock *= bclkRatio;
    const uint32_t bitcnt = (clock / rate - 1) >> 1;
    const uint32_t timing = bitcnt | (clock % (2 * rate) ? 1u << 12 : 0);
    return {bits(sampleSize), clock, timing};
}

// rt5640.h fields.
constexpr uint32_t kI2sMsS = 1u << 15, kI2sBpNor = 0, kI2sDfI2s = 0;
constexpr uint32_t kSdpFmtMask = (1u << 15) | (1u << 7) | 3;  // rt5640_set_dai_fmt: MS, BP, DF
constexpr uint32_t kI2sDlMask = 3u << 2;
constexpr uint32_t kI2sBclkMs1Sft = 15, kI2sPd1Sft = 12;
constexpr uint32_t kAddaMask = (1u << 15) | (7u << 12);

constexpr std::optional<uint32_t> i2sDl(unsigned width)
{
    switch (width) {
    case 16: return 0;
    case 20: return 1u << 2;
    case 24: return 2u << 2;
    case 8: return 3u << 2;
    default: return std::nullopt;
    }
}

// rl6231.c: the pre-divider index with sclk == rate * 256 * pd.
constexpr std::optional<uint32_t> rl6231ClkInfo(uint32_t sclk, uint32_t rate)
{
    const uint32_t pds[] = {1, 2, 3, 4, 6, 8, 12, 16};
    for (uint32_t i = 0; i < 8; ++i)
        if ((uint64_t)sclk == ((uint64_t)rate << 8) * pds[i]) return i;
    return std::nullopt;
}

struct Rt5640 {
    uint32_t sdpMask, sdp, addaMask, adda, bclkFs;
};

// rt5640_set_dai_fmt (CBS_CFS, I2S, NB_NF) then rt5640_hw_params for IF1; nullopt where the driver gives -EINVAL.
constexpr std::optional<Rt5640> rt5640(unsigned width, uint32_t channels, uint32_t rate, uint32_t sysclk)
{
    const std::optional<uint32_t> dl = i2sDl(width);
    if (!dl) return std::nullopt;                 // rt5640_hw_params: -EINVAL for this width
    const uint32_t frameSize = width * channels;  // snd_soc_params_to_frame_size
    const uint32_t bclkMs = frameSize > 32 ? 1 : 0;
    const std::optional<uint32_t> preDiv = rl6231ClkInfo(sysclk, rate);
    if (!preDiv) return std::nullopt;  // no pre-divider
    const uint32_t sdp = kI2sMsS | kI2sBpNor | kI2sDfI2s | *dl;
    const uint32_t adda = bclkMs << kI2sBclkMs1Sft | *preDiv << kI2sPd1Sft;
    return Rt5640{kSdpFmtMask | kI2sDlMask, sdp, kAddaMask, adda, 32u << bclkMs};
}

// An exact fraction (Python's fractions.Fraction for the clock checks), always reduced, denominator positive.
struct Fraction {
    __int128 n, d;
    static __int128 gcd(__int128 a, __int128 b)
    {
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        while (b) {
            const __int128 t = a % b;
            a = b;
            b = t;
        }
        return a;
    }
    Fraction(__int128 num = 0, __int128 den = 1) : n(num), d(den)
    {
        if (d < 0) n = -n, d = -d;
        const __int128 g = gcd(n, d);
        if (g > 1) n /= g, d /= g;
    }
    friend Fraction operator*(const Fraction &a, const Fraction &b) { return {a.n * b.n, a.d * b.d}; }
    friend Fraction operator/(const Fraction &a, const Fraction &b) { return {a.n * b.d, a.d * b.n}; }
    friend Fraction operator+(const Fraction &a, const Fraction &b) { return {a.n * b.d + b.n * a.d, a.d * b.d}; }
    friend bool operator==(const Fraction &a, const Fraction &b) { return a.n == b.n && a.d == b.d; }
    // floor(this), as Python's math.floor of a Fraction.
    __int128 floor() const { return n >= 0 ? n / d : -((-n + d - 1) / d); }
    double value() const { return (double)n / (double)d; }
};

}  // namespace switch_audio_linux
