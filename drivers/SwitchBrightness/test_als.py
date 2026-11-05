"""Host check of the ambient light sensor path: SwitchALSMath.h against Linux's maths, and the I2C2 pad, bus and command
values in TegraI2C.cpp and SwitchALS.cpp against the references they cite.

1. SwitchALSMath.h, compiled for the Mac, against a port of CTCaer/switch-l4t-kernel-4.9 drivers/iio/light/bh1730.c
   (bh1730_get_lux, bh1730_adjust_gain) with the OLED's calibration (tegra210-nx-sensor.dtsi: integration cycle 38,
   lux multiplier 3600; tegra210b01-fric.dts: opt-win-coeff, gain-coeff), on a grid of both channels at every gain:
   lux never below Linux's and at most 4 above (Linux truncates three times, we once), never a u64 wrap inside a row;
   the gain step equal to Linux's except where Linux would step up into a reading over the next gain's fl, and no
   step up that the next sample undoes. Row 0's ci is hekate's Aula 30300, not the DT's 7502: every row boundary of
   Nintendo's Icosa table (hekate's default) and of the Aula table with 30300 is continuous, and 7502 is not. The
   scale against the datasheet's bare sensor (window loss) at r7r's readings, for ours and for the Icosa table.
2. TegraI2C.cpp's I2C2 row and pad values: Atmosphere i2c_bus_device_map.inc (I2c2 0x7000C400, fast mode, interrupt
   116), pinmux_initial_pad_config_aula.inc (Gen2I2cScl/Sda opt 0x40, mask 0x27F), pinmux_initial_drive_pad_config.inc
   (Gen2I2c 0x0001F000, mask 0x01F1F000) and pinmux_drive_pad_characters.inc (0x09B0, 0x09B4); Linux
   pinctrl-tegra210.c (gen2_i2c_scl_pj2 0x30c4, gen2_i2c_sda_pj3 0x30c8); L4T tegra210b01-fric-pinmux.dtsi (function
   i2c2, no pull, not tristate, input, io-high-voltage off; pull-down strength 0x1F, pull-up 0); hekate clock.c
   (I2C2: RST_DEV_H_SET, CLK_ENB_H_SET, CLK_SOURCE_I2C2 0x198, bit 22, CLK_M / 1 for 400 kHz).
3. SwitchALS.cpp's BH1730FVC command bytes against the ROHM datasheet (TSZ02201-0M3M0F616050-1-2): COMMAND = 1, TRANSACTION
   00 + register or 11 + special command (software reset 00100), PART_ID at 12h, DATA0 at 14h, TIMING default DAh.
4. What CoreBrightness reads (21H461 disassembly): the light sensor's personality keys in SwitchHIDDevice.cpp
   (-[CBALSNode initALSProperties]: Placement, Orientation, the three integration times; IOHIDFamily
   AppleEmbeddedHIDKeys.h enums) and SwitchBacklight.cpp's backlight-calibration-parameters (DisplayCreateWithID: all
   nine mandatory keys, 16.16 fixed point, nits in order).
5. SwitchBacklightMath.h, compiled for the Mac: brightness-nits (nits * 65536) onto the panel linearly in luminance,
   2047 at lMaxPanel and never below the floor, which is lMinPanel; the legacy brightness mapping unchanged; the
   brightness the ramp keeps lands on the same panel level; the r7s auto-brightness targets it dropped.
6. Why r7v's auto-brightness did not move from 61 to 135 lux: CoreBrightness's curve (backboardd's ALSUserPreference,
   E1 0 / L1 63.81 to E2 2000 / L2 287.62 nits above E0b 30 lux) gives the logged target at 61 lux exactly, and the
   step to 135 lux is 0.03 in its perceptual scale (BULuminanceToPerceptual: 1 + ln(L / Lmax) / ln(Lmax / Lmin), over
   this panel's 12.5..400 nits), a third of the brighten threshold 0.2 * 0.5 (AABC e70 * aggressivity 0x2a4; inferred).
7. r7w's finger-then-flashlight test, through SwitchALS::sample's rule (a clipped reading above 1x only steps the gain
   down, the sample after a gain change is skipped): 0 lux, then 50000, then 0 again, starting at 64x. Readings come back
   valid within a few samples, no clipped reading is dispatched, and nothing in between under-reads. And SwitchBacklight
   publishes new-backlight-architecture, without which CoreBrightness's faded ramps never reach brightness-nits.
8. DisplayCreateWithID's ways to fail that are this service's (21H461): IODisplayParameters (0x1b62acf74) and, under the
   new architecture, backlight-marketing-table (0x1b62ae660; r7y/r7z had none: no display, so libEDR's server never
   started and Core Animation's render server stalled 5 s per frame on its port). SwitchBacklight.cpp publishes both
   before backlight-control and backlight-control only with IODisplayParameters, new-backlight-architecture only with
   the table. The table, compiled from SwitchBacklightMath.h and read as DisplayCreateWithID reads it (int32 / 65536
   into float), is the calibration's product range, needs no digital-dimming rescale (0x5e4/0x5e5 stay clear), and
   through __DisplaySliderToLogicalBrightnessInternal puts the slider's ends on the panel's floor and full level.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent
drivers = root.parent

# ---- 1. maths ------------------------------------------------------------------------------------------------------
U64 = (1 << 64) - 1
CYCLES, MULTIPLIER, MAX_LUX = 38, 3600, 100000
ITIME_US = 2700 * CYCLES
GAINS = [1, 2, 64, 128]
WINDOW = [(231, 9697, 30300), (993, 3333, 2778), (1478, 1621, 1053), (7500, 81, 10)]  # hekate Aula = fric.dts but ci0
DT_WINDOW = [(231, 9697, 7502)] + WINDOW[1:]                                           # tegra210b01-fric.dts as is
ICOSA_WINDOW = [(500, 5002, 7502), (754, 2250, 2000), (1029, 1999, 1667), (1373, 884, 583), (1879, 309, 165)]  # hekate
GAIN_COEFF = [(2500, 0), (1600, 7800), (10, 50000), (0, 1000)]                      # (cl, fl), tegra210b01-fric.dts


def linux_lux(visible, ir, gain):
    """bh1730_get_lux: u64 arithmetic, three truncating steps."""
    if visible <= 0:
        return 0
    lux = 0
    for rc, cv, ci in WINDOW:
        if 1000 * ir // visible < rc:
            assert cv * visible >= ci * ir, (visible, ir)  # Linux would wrap in u64 here
            lux = (cv * visible - ci * ir) & U64
            break
    lux = (lux * 100) & U64
    lux //= GAINS[gain] * ITIME_US
    lux = (lux * MULTIPLIER) // 1000
    return min(lux, MAX_LUX)


def linux_gain(gain, visible, ir):
    """bh1730_adjust_gain."""
    highest = max(visible, ir)
    cl, fl = GAIN_COEFF[gain]
    if highest > fl and gain != 0:
        return gain - 1
    if highest < cl and gain != 3:
        return gain + 1
    return gain


values = sorted(set(list(range(0, 64)) + [int(1.07 ** k) for k in range(64, 165)] + [100, 132, 372, 1000, 65534, 65535]))
harness = r'''
#include <cstdio>
#include "SwitchALSMath.h"
using namespace switch_als;
int main() {
    static const unsigned values[] = {%s};
    for (unsigned g = 0; g < kGains; ++g)
        for (unsigned v : values)
            for (unsigned i : values) {
                const unsigned next = nextGain(g, v, i);
                // A step up must hold at the next sample: the same light, read at the higher gain.
                if (next > g) {
                    const unsigned long scale = kGainFactor[next] / kGainFactor[g];
                    const unsigned v2 = v * scale > 65535 ? 65535 : (unsigned)(v * scale);
                    const unsigned i2 = i * scale > 65535 ? 65535 : (unsigned)(i * scale);
                    if (nextGain(next, v2, i2) < next) { printf("FLIP %%u %%u %%u\n", g, v, i); return 1; }
                }
                printf("%%u %%u %%u %%u %%u\n", g, v, i, lux(v, i, g), next);
            }
    return 0;
}
''' % ', '.join(str(v) for v in values)

with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'als.cpp').write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(work / 'als.cpp'), '-o',
                    str(work / 'als')], check=True)
    output = subprocess.run([str(work / 'als')], check=True, capture_output=True, text=True).stdout

rows = 0
worst = 0
saturated_cases = 0
for line in output.splitlines():
    g, v, i, ours, next_gain = map(int, line.split())
    if v >= 65535 or i >= 65535:
        # Clipped: the ceiling at 1x, no reading (the gain steps down) above it.
        assert ours == (MAX_LUX if g == 0 else 0), (g, v, i, ours)
        assert g == 0 or next_gain == g - 1, (g, v, i, next_gain)
        saturated_cases += 1
        rows += 1
        continue
    reference = linux_lux(v, i, g)
    assert reference <= ours <= reference + 4, (g, v, i, ours, reference)
    worst = max(worst, ours - reference)
    stepped = linux_gain(g, v, i)
    if stepped > g:
        cl, fl = GAIN_COEFF[stepped]
        held = max(v, i) * GAINS[stepped] // GAINS[g] < fl
        assert next_gain == (stepped if held else g), (g, v, i, next_gain, stepped)
    else:
        assert next_gain == stepped, (g, v, i, next_gain, stepped)
    rows += 1
unclipped = sum(v < 65535 for v in values)
assert rows == 4 * len(values) ** 2 and saturated_cases == 4 * (len(values) ** 2 - unclipped ** 2)
# Spot value: 64x, DATA0 1000, DATA1 100 (ratio 100 < 231, row 0): (9697000 - 3030000) * 100 * 3600 / (64 * 102600 *
# 1000) = 365.5; Linux's 6667000 * 100 // 6566400 = 101, * 3600 // 1000 = 363.
assert linux_lux(1000, 100, 2) == 363
assert '\n2 1000 100 365 ' in '\n' + output


def joins(table):
    """Each row's line cv - ci * r meets the next row's at the boundary r = rc (per mille), within 0.2% of its cv
    (the published tables are rounded to integers: the worst join is 1.6 in 1621)."""
    return [abs((cv - ci * rc / 1000) - (cv2 - ci2 * rc / 1000)) <= 0.002 * cv
            for (rc, cv, ci), (_, cv2, ci2) in zip(table, table[1:])]


assert all(joins(ICOSA_WINDOW)) and all(joins(WINDOW))
assert joins(DT_WINDOW) == [False, True, True]  # 7964 against 2691 at 231


def window_lux(table, d0, d1, gain=2):
    for rc, cv, ci in table:
        if 1000 * d1 / d0 < rc:
            return (cv * d0 - ci * d1) * 100 * MULTIPLIER / (GAINS[gain] * ITIME_US * 1000)
    return 0


# Scale: r7r's uncovered reading, 64x DATA0 372 DATA1 132 (ratio 0.355), against the datasheet's own formula for the bare
# part (ratio < 0.55: (0.795 DATA0 - 0.859 DATA1) / gain * 102.6 / ITIME ms): the optical window's factor.
bare = (0.795 * 372 - 0.859 * 132) / 64
assert 16.6 < window_lux(WINDOW, 372, 132) / bare < 17.0 and 16.6 < window_lux(ICOSA_WINDOW, 372, 132) / bare < 17.0
assert '\n2 372 132 47 ' in '\n' + output  # r7r logged lux 47 for ch0 372 ch1 132 at 64x

# ---- 2. I2C2 bus and pads -----------------------------------------------------------------------------------------
i2c = (drivers / 'TegraI2C' / 'TegraI2C.cpp').read_text()
table = {}
for match in re.finditer(r'\{([0-9a-fx, ]+)\},\s*// I2C(\d)', i2c):
    table[int(match.group(2))] = [int(field, 0) for field in match.group(1).split(',')]
assert sorted(table) == [1, 2, 3, 4, 5], table
regs, reset, enable, source, reset_state, enable_state, bit, divider, scl, sda, spi, scl_drive, sda_drive, pull_down = \
    table[2]
assert 0x7000C000 + regs == 0x7000C400                       # Atmosphere I2c2 registers
assert (reset, enable, source, bit) == (0x308, 0x328, 0x198, 22)  # hekate clock.c _clock_i2c[1]
assert (reset_state, enable_state) == (0x008, 0x014)          # RST_DEVICES_H, CLK_OUT_ENB_H
assert 19200000 // divider // 48 == 400000                    # SpeedMode_Fast; L4T clock-frequency 400000
assert spi + 32 == 116                                        # Atmosphere interrupt 116; tegra210.dtsi GIC_SPI 84
assert (0x3000 + scl, 0x3000 + sda) == (0x30C4, 0x30C8)        # pinctrl-tegra210.c gen2_i2c_scl_pj2 / sda_pj3
assert (scl_drive, sda_drive) == (0x9B0, 0x9B4)                # pinmux_drive_pad_characters.inc Gen2I2cScl/Sda

pinmux = int(re.search(r'kPinmuxI2C = 1u << (\d+);', i2c).group(1))
pinmux = 1 << pinmux
# Atmosphere opt 0x40 (Bidirection) under mask 0x27F sets PM 0 (I2C2), PUPD 0, TRISTATE 0, E_INPUT 1, E_OD 0 (the pad
# has it: characters mask 0x01DFF) and E_IO_HV 0 (NormalVoltage); everything else it leaves as found.
HORIZON_FIELDS = 0x3 | 0xC | 0x10 | 0x40 | 0x800 | 0x400
assert pinmux & HORIZON_FIELDS == 0x40
assert pinmux & ~HORIZON_FIELDS == 0  # we also clear PARK, LOCK, E_LPDR, E_SCHMT, as hekate pinmux_config_i2c

mask = eval(re.search(r'kDriveMask = (\(0x1fu << 20\) \| \(0x1fu << 12\));', i2c).group(1).replace('u', ''))
assert mask == 0x01F1F000 and pull_down == 0x1F
for old in (0, 0xFFFFFFFF, 0xF0F0F0F0, 0x00004000, 0x12345678):
    ours = (old & ~mask & 0xFFFFFFFF) | pull_down << 12
    horizon = (old & ~0x01F1F000 & 0xFFFFFFFF) | 0x0001F000  # reg::ReadWrite(value, mask)
    assert ours == horizon, hex(old)
assert 'if (info.sclDrive != kBus[1].sclDrive) return;' in i2c  # the drive is I2C2's only

# ---- 3. BH1730FVC commands -----------------------------------------------------------------------------------------
als = (root / 'SwitchALS.cpp').read_text()
assert 'kSensor = 0x29;' in als and 'kBus = 2;' in als                       # slave address 0101001, I2C2
assert 'kCommand = 0x80, kSoftwareReset = 0xe4;' in als                     # 1 00 xxxxx; 1 11 00100
assert 'RegControl = 0x00, RegTiming = 0x01, RegGain = 0x07, RegId = 0x12, RegData0 = 0x14' in als
assert 256 - CYCLES == 0xDA                                                  # TIMING default DAh: the 38 cycles
assert 'tegra_i2c_write(kBus, kSensor, kSoftwareReset, nullptr, 0)' in als  # special command alone

# ---- 4. What CoreBrightness reads -----------------------------------------------------------------------------------
hid = (drivers / 'SwitchTouch' / 'SwitchHIDDevice.cpp').read_text()
light = dict((k, int(v)) for k, v in re.findall(r'\{"(Placement|Orientation|ALS\w+IntegrationTime)", (\d+)\}', hid))
assert light == {'Placement': 1,                                # kIOHIDPlacementTypeTop; 2 (Bottom) is the rear ALS
                 'Orientation': 8,                              # kIOHIDOrientationTypeSouthWest: portrait bottom left
                 'ALSFastIntegrationTime': ITIME_US, 'ALSSuperFastIntegrationTime': ITIME_US,
                 'ALSSlowIntegrationTime': ITIME_US}, light
assert 'if (&p == &kProfiles[kSwitchHIDLight]) addLightProperties(dict);' in hid
backlight = (root / 'SwitchBacklight.cpp').read_text()
keys = re.findall(r'\{"([a-zA-Z-]+)", kNits(Max|Mid|Min)\}', backlight)
assert sorted(k for k, _ in keys) == sorted(['current-for-max-backlight', 'current-for-mid-backlight',
                                             'hardware-max-current-limit', 'lMaxProduct', 'lMidProduct', 'lMinProduct',
                                             'lMaxPanel', 'lMidPanel', 'lMinPanel']), keys
assert dict(keys)['lMaxPanel'] == 'Max' and dict(keys)['lMinPanel'] == 'Min' and dict(keys)['lMidPanel'] == 'Mid'
assert 'OSNumber::withNumber(entry.value, 32)' in backlight  # already nits * 65536 (read as float / 65536)
assert 'OSNumber *nits = OSDynamicCast(OSNumber, dict->getObject("brightness-nits"));' in backlight
assert 'value = nitsToBrightness(requested);' in backlight

# ---- 5. The panel model ----------------------------------------------------------------------------------------------
PANEL, FLOOR = 2047, 64
NITS_MAX = 400 << 16
r7s = [37.7215, 47.7215, 51.0549, 84.3882, 137.9125, 103.5238]  # r7s Begin ramp L_target values, never shown
probe = [int(n * 65536) for n in r7s] + list(range(-65536, 420 << 16, 4099)) + [0, NITS_MAX, NITS_MAX + 1, 0x7fffffff]
model = r'''
#include <cstdio>
#include "SwitchBacklightMath.h"
using namespace switch_backlight;
int main() {
    static const long long probe[] = {%s};
    printf("consts %%u %%u %%u %%u %%u\n", kNitsMax, kNitsMid, kNitsMin, toRaw(0), toRaw(kMax));
    for (unsigned raw = 0; raw <= kPanelMax + 2; ++raw) {
        if (raw >= kFloorRaw && raw <= kPanelMax && toRaw(toBrightness(raw)) != raw) { printf("RT %%u\n", raw); return 1; }
    }
    for (long long n : probe) {
        const int32_t v = (int32_t)n;
        printf("%%d %%u %%u %%u\n", v, nitsToRaw(v), toRaw(nitsToBrightness(v)), rawToNits(nitsToRaw(v)));
    }
    return 0;
}
''' % ', '.join(str(n) for n in probe)
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'bl.cpp').write_text(model)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(work / 'bl.cpp'), '-o',
                    str(work / 'bl')], check=True)
    lines = subprocess.run([str(work / 'bl')], check=True, capture_output=True, text=True).stdout.splitlines()
consts = list(map(int, lines[0].split()[1:]))
assert consts[:2] == [400 << 16, 140 << 16]
assert consts[2] == round((400 << 16) * FLOOR / PANEL) and abs(consts[2] / 65536 - 12.506) < 0.01  # lMinPanel = floor
assert consts[3:] == [FLOOR, PANEL]  # the legacy brightness mapping, as before
shown = {}
previous = None
for line in lines[1:]:
    v, raw, ramp, back = map(int, line.split())
    expect = min(PANEL, max(FLOOR, (v * PANEL + NITS_MAX // 2) // NITS_MAX)) if v > 0 else FLOOR  # nearest level
    assert raw == expect, (v, raw, expect)
    assert ramp == raw, (v, raw, ramp)              # the ramp's brightness shows that level
    assert abs(back - raw * NITS_MAX / PANEL) <= 1  # the readback is that level's nits
    if previous is not None and v >= previous[0]:
        assert raw >= previous[1], (v, raw, previous)  # monotonic
    previous = (v, raw)
    shown[v] = raw
targets = [shown[int(n * 65536)] for n in r7s]
assert targets == [193, 244, 261, 432, 706, 530], targets  # 38..138 nits: panel 193..706 (r7s stayed at 525)
assert shown[0] == FLOOR and shown[NITS_MAX] == PANEL and shown[0x7fffffff] == PANEL

# ---- 6. r7v's auto-brightness -------------------------------------------------------------------------------------
import math
E1, L1, E2, L2, E0B, L0A, L0B = 0, 63.80889892578125, 2000, 287.6190490722656, 30, 5.000001907348633, 67.16605377197266


def aab_curve(lux):
    if lux < E0B:
        return L0A + (L0B - L0A) * lux / E0B
    return L1 + (L2 - L1) * (lux - E1) / (E2 - E1)


def perceptual(nits, low=consts[2] / 65536, high=400.0):
    return 1 + math.log(max(nits, low) / high) / math.log(high / low)


assert abs(aab_curve(61) - 70.6351) < 5e-5  # r7v: Begin ramp L_target=70.6351 _Esensor_trusted=61
delta = perceptual(aab_curve(135)) - perceptual(aab_curve(61))
assert 0.025 < delta < 0.04, delta          # well under 0.1: no ramp, as iOS does
threshold = 0.2 * 0.5
brighten = next(lux for lux in range(61, 2001) if perceptual(aab_curve(lux)) - perceptual(aab_curve(61)) > threshold)
dim = next(lux for lux in range(61, -1, -1) if perceptual(aab_curve(61)) - perceptual(aab_curve(lux)) > threshold)
assert 250 < brighten < 400 and 5 < dim < 25, (brighten, dim)

# ---- 7. Steps through saturation, and the backlight architecture ---------------------------------------------------
assert 'setProperty("new-backlight-architecture", kOSBooleanTrue);' in backlight
step = r'''
#include <cstdio>
#include "SwitchALSMath.h"
using namespace switch_als;
// Counts the part gives for `lux` at a DATA1/DATA0 ratio of 0.37 (row 1), clipped at the ADC's top.
static void counts(double lux, unsigned gain, unsigned &visible, unsigned &ir) {
    const double v = lux * kGainFactor[gain] * kIntegrationUS * 1000.0 / ((3333 - 2778 * 0.37) * 100.0 * kLuxMultiplier);
    visible = v > 65535 ? 65535 : (unsigned)v;
    const double i = v * 0.37;
    ir = i > 65535 ? 65535 : (unsigned)i;
}
int main() {
    const double scene[] = {0, 50000, 0};
    unsigned gain = 2, skip = 0;  // 64x, as SwitchALS starts
    for (unsigned phase = 0; phase < 3; ++phase)
        for (unsigned tick = 0; tick < 12; ++tick) {
            if (skip) { --skip; printf("%u %u skip %u\n", phase, tick, gain); continue; }
            unsigned v, i;
            counts(scene[phase], gain, v, i);
            if (!saturated(v, i) || gain == 0) printf("%u %u lux %u %u\n", phase, tick, lux(v, i, gain), gain);
            else printf("%u %u clipped %u\n", phase, tick, gain);
            const unsigned next = nextGain(gain, v, i);
            if (next != gain) { gain = next; skip = 1; }
        }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'step.cpp').write_text(step)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(work / 'step.cpp'), '-o',
                    str(work / 'step')], check=True)
    trace = [line.split() for line in subprocess.run([str(work / 'step')], check=True, capture_output=True,
                                                    text=True).stdout.splitlines()]
for phase, want in ((0, 0), (1, 50000), (2, 0)):
    events = [t for t in trace if int(t[0]) == phase]
    first = next(int(t[1]) for t in events if t[2] == 'lux')
    assert first <= 4, (phase, events[:6])                              # a valid reading within four samples
    readings = [int(t[3]) for t in events if t[2] == 'lux']
    assert all(abs(r - want) <= max(2, want // 50) for r in readings), (phase, readings)  # never an under-read
    assert events[-1][2] == 'lux', events[-1]                          # and it stays valid
assert any(t[2] == 'clipped' for t in trace if int(t[0]) == 1)          # 50000 lux clips 64x: dropped, not dispatched
settled = {int(t[0]): int(t[4]) for t in trace if t[2] == 'lux'}
assert settled == {0: 3, 1: 0, 2: 3}, settled                            # 128x in the dark, 1x at 50000 lux

# ---- 8. DisplayCreateWithID gets its display: what it needs before backlight-control ------------------------------
import struct

start = backlight[backlight.index('bool SwitchBacklight::start(IOService *provider)'):]
start = start[:start.index('\n}\n')]
order = ['publishParameters();', 'setProperty("backlight-calibration-parameters", parameters);',
         'setProperty("backlight-marketing-table", table)', 'setProperty("new-backlight-architecture", kOSBooleanTrue);',
         'setProperty("backlight-control", kOSBooleanTrue);', 'registerService();']
assert all(s in start for s in order), [s for s in order if s not in start]  # backlight-marketing-table: r7y/r7z
at = [start.index(s) for s in order]
assert at == sorted(at), list(zip(order, at))  # everything DisplayCreateWithID reads is there before backlight-control
for key in ('backlight-control', 'new-backlight-architecture', 'backlight-marketing-table'):
    assert backlight.count(f'setProperty("{key}"') == 1, key  # published once, in start()
assert re.search(r'const bool marketing = table && setProperty\("backlight-marketing-table", table\);\s*'
                 r'OSSafeReleaseNULL\(table\);\s*if \(marketing\) \{\s*setProperty\("new-backlight-architecture"',
                 start)  # the new architecture only with its table (0x1b62ae660)
assert re.search(r'if \(OSDynamicCast\(OSDictionary, getProperty\("IODisplayParameters"\)\)\) \{\s*'
                 r'setProperty\("backlight-control", kOSBooleanTrue\);', start)  # 0x1b62acf74
assert 'OSData::withBytes(kMarketingTable, sizeof(kMarketingTable))' in start  # CFData (0x1b62ae0a8 drops others)

table_model = r'''
#include <cstdio>
#include "SwitchBacklightMath.h"
using namespace switch_backlight;
int main() {
    const unsigned char *bytes = (const unsigned char *)kMarketingTable;
    printf("%zu", sizeof(kMarketingTable));
    for (size_t i = 0; i < sizeof(kMarketingTable); ++i) printf(" %u", bytes[i]);
    printf("\n%u %u %u\n", kNitsMin, kNitsMid, kNitsMax);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'table.cpp').write_text(table_model)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(work / 'table.cpp'), '-o',
                    str(work / 'table')], check=True)
    out = subprocess.run([str(work / 'table')], check=True, capture_output=True, text=True).stdout.splitlines()
size, *raw_bytes = map(int, out[0].split())
assert size == len(raw_bytes) and size % 4 == 0 and size // 4 >= 2, out[0]  # CFDataGetLength / 4 entries, two or more
table = list(struct.unpack('<%di' % (size // 4), bytes(raw_bytes)))  # little-endian int32, as ldr w8 / scvtf reads it
n_min, n_mid, n_max = map(int, out[1].split())
assert table == [n_min, n_mid, n_max], table  # the calibration's product range: min, mid, max


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


marketing = [f32(f32(v) / 65536) for v in table]  # scvtf s0, w8; fdiv by 65536.0 (0x1b62ae128..0x1b62ae138)
assert all(a < b for a, b in zip(marketing, marketing[1:])), marketing
# display+0x50c/0x510/0x514 = lMax/Mid/MinProduct, 0x518/0x51c/0x520 = lMax/Mid/MinPanel (0x1b62ad334..0x1b62ad460).
product = [f32(n_max / 65536), f32(n_mid / 65536), f32(n_min / 65536)]
panel = product[:]  # calibration(): the same values
# digital-dimming-min-nits / user-accessible-max-nits absent: their defaults are the panel's min and max, through
# fcvtzu(x * 65536) / 65536 (0x1b62adf64..0x1b62ae00c); the rescale loops run only if they come out lower (0x5e4/0x5e5).
for limit in (panel[2], panel[0]):
    assert f32(int(f32(limit * 65536.0)) / 65536.0) == limit, limit  # no rescale: the table is used as published


def interpolate(x0, y0, x1, y1, x):
    return f32(y0 + (x - x0) * (y1 - y0) / (x1 - x0))


def slider_to_nits(slider):  # __DisplaySliderToLogicalBrightnessInternal, 0x1b62b29a0..0x1b62b2b74
    position = f32(slider * (len(marketing) - 1))
    index = int(position)
    if index < len(marketing) - 1:
        m = interpolate(index, marketing[index], index + 1, marketing[index + 1], position)
    else:
        m = marketing[-1]
    if m < product[1]:
        return interpolate(product[2], panel[2], product[1], panel[1], m)
    return interpolate(product[1], panel[1], product[0], panel[0], m)


curve = [slider_to_nits(i / 100) for i in range(101)]
assert all(a <= b for a, b in zip(curve, curve[1:])), curve  # monotonic
assert curve[0] == panel[2] and curve[50] == panel[1] and curve[100] == panel[0], (curve[0], curve[50], curve[100])
ends = [min(PANEL, max(FLOOR, (int(n * 65536) * PANEL + NITS_MAX // 2) // NITS_MAX)) for n in (curve[0], curve[100])]
assert ends == [FLOOR, PANEL], ends  # nitsToRaw (section 5): the slider's bottom is the panel's floor, its top full
assert abs(curve[0] - 12.506) < 0.01 and curve[100] == 400.0

print(f'SwitchALS: {rows} lux/gain cases match bh1730.c (worst +{worst} lux); I2C2 row, pinmux 0x40, drive DRVDN 0x1f '
      f'and BH1730 commands match Atmosphere, L4T, Linux, hekate and the datasheet; window tables continuous; '
      f'CoreBrightness ALS and backlight calibration keys present; brightness-nits {targets} on the panel; r7v at 61 lux '
      f'holds from {dim} to {brighten} lux (perceptual step to 135 lux {delta:.3f}); DisplayCreateWithID has '
      f'IODisplayParameters and backlight-marketing-table {[round(m, 2) for m in marketing]} nits before '
      f'backlight-control, slider ends on panel {ends}')
