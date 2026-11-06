// thermalmonitord's sensors on the Switch (drivers/SwitchPower/SwitchSensors.h, CONTRACT.md next to it).
//
// Checks, against what iPadOS 21H461 itself has (recorded below from /System/Library/ThermalMonitor/J120AP-Info.plist
// and the thermalmonitord binary, ThermalMonitor-1952.120.7):
//   * the sensor table is the plist's non-synth Sensors entries, same order, 4CCs and matchNames, so the watchdog mask
//     of those indices is the 0x3fffffffff thermalmonitord logged as "expected";
//   * every mapping is a real measurement of the nearest point: CPU-complex sensors from SOC_THERM's per-core tsensors
//     (core n from cpu n), SoC die sensors from on-die points only, battery sensors from the battery thermistor, board
//     thermistors from the PCB channel, tcal from the identity calibration;
//   * unit conversions (SOC_THERM readback, MAX17050 TEMP, IOFixed, thermalmonitord's centi-degree validity window),
//     the MAX17050 alert window;
//   * SOC_THERM's T210B01 fuse calibration against an independent model of L4T's tsensor-fuse.c;
//   * CLTM's CPU ceiling: the DVD factor keys, the lowest-contributor rule and the governor index it maps to.
// The HID shapes and vtable slots the kernel side relies on: switch_sensors_hid_source_test.cpp.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/require.h"
#include "switch_thermal_sensors_harness.h"

namespace {

using Sensor = std::pair<std::string, std::string>;  // 4CC, matchName

// J120AP-Info.plist "Sensors", 21H461: every entry without synthSensor (the 5 synth ones, SS0F SS0C TSBH TS0A TSFD,
// have requiresWatchDog false and are computed by thermalmonitord itself).
std::vector<Sensor> j120apHidSensors()
{
    std::vector<Sensor> sensors = {
        {"TG0B", "gas gauge battery"}, {"TP1d", "PMU tdev1"},    {"TP2d", "PMU tdev2"},  {"TP3d", "PMU tdev3"},
        {"TP4d", "PMU tdev4"},         {"TP5d", "PMU tdev5"},    {"TP6d", "PMU tdev6"},  {"TP7d", "PMU tdev7"},
        {"TP8d", "PMU tdev8"},         {"TR1d", "PMU2 tdev1"},   {"TR2d", "PMU2 tdev2"}, {"TP0Z", "PMU tcal"},
        {"TP0j", "PMU tjunc"},         {"TQ0B", "Charger tbat"},
    };
    const std::pair<const char *, const char *> kinds[] = {{"i", ""}, {"a", "Avg: "}, {"x", "Max: "}};
    for (int n = 0; n < 4; ++n)
        for (const auto &[k, p] : kinds)
            sensors.emplace_back("Th" + std::to_string(n) + k,
                                 std::string(p) + "PMGR SOC Die Temp Sensor" + std::to_string(n));
    for (int n = 0; n < 4; ++n)
        for (const auto &[k, p] : kinds)
            sensors.emplace_back("Tc" + std::to_string(n) + k, std::string(p) + "ACC Temp Sensor" + std::to_string(n));
    return sensors;
}

constexpr uint64_t kExpectedMask = 0x3fffffffffull;  // "is_alive_func returned unhealthy : current 0, mask 3fffffffff,
                                                     // expected 3fffffffff"

// SwitchSensors.h switch_sensor_source
constexpr int kSocCpu[4] = {0, 1, 2, 3};
constexpr int kSocGpu = 4, kSocPllx = 5, kTmp451Soc = 6, kTmp451Pcb = 7, kBattery = 8, kTcal = 9;
const std::set<int> kDiePoints = {kSocGpu, kSocPllx, kTmp451Soc};

// MitigationController's CPU keys (thermalmonitord __DATA_CONST 0x10007dbd8..0x10007dbf0, CFStrings).
const std::vector<std::string> kCltmKeys = {
    "AppleARMPerformanceControllerDVDFactor1-slowloop", "AppleARMPerformanceControllerDVDFactor1-hip",
    "AppleARMPerformanceControllerDVDFactor1-upo", "AppleARMPerformanceControllerDVDFactor1-lpm"};

// L4T tegra210-soctherm.c tegra210b01_tsensors / tegra210b01_tsensor_config.
struct Tsensor {
    const char *name;
    uint32_t base, fuse;
    int64_t alpha, beta;
};

constexpr Tsensor kB01Tsensors[] = {{"cpu0", 0xc0, 0x098, 1085000, 3244200},  {"cpu1", 0xe0, 0x084, 1126200, -67500},
                                    {"cpu2", 0x100, 0x088, 1098400, 2251100}, {"cpu3", 0x120, 0x12c, 1108000, 602700},
                                    {"gpu", 0x180, 0x154, 1074300, 2734900},  {"pllx", 0x1a0, 0x160, 1039700, 6829100}};

constexpr struct {
    int64_t tall = 16300, tiddqEn = 1, tenCount = 1, tsample = 240, tsampleAte = 480, pdiv = 12, pdivAte = 6;
} kB01;

int64_t s32(int64_t x)
{
    return (int32_t)(uint32_t)(uint64_t)x;
}

int64_t s16(int64_t x)
{
    return (int16_t)(uint16_t)(uint64_t)x;
}

int64_t signExtend32(uint32_t value, int index)
{
    const int shift = 31 - index;
    return (int32_t)(value << shift) >> shift;
}

// tsensor-fuse.c div64_s64_precise: s64 a, s32 b (C's truncating division, then an arithmetic shift).
int64_t div64S64Precise(int64_t a, int64_t b)
{
    b = s32(b);
    return (a * 65536 * 2 + 1) / (2 * b) >> 16;
}

// tegra_calc_shared_calib + tegra_calc_tsensor_calib, tegra210_soctherm_fuse, T210B01 config.
std::optional<uint32_t> l4tCalib(uint32_t common, uint32_t fuse, int64_t alpha, int64_t beta)
{
    const int64_t baseCp = (common & (0x3ffu << 11)) >> 11;
    const int64_t baseFt = (common & (0x7ffu << 21)) >> 21;
    const int64_t shiftedFt = signExtend32((common & (0x1fu << 6)) >> 6, 4);
    const int64_t shiftedCp = signExtend32(common, 5);
    const int64_t actualCp = 2 * 25 + shiftedCp, actualFt = 2 * 105 + shiftedFt;
    const int64_t cp = baseCp * 64 + signExtend32(fuse, 12);
    const int64_t ft = baseFt * 32 + signExtend32((fuse & (0x1fffu << 13)) >> 13, 12);
    const int64_t deltaSens = ft - cp, deltaTemp = actualFt - actualCp;
    if (deltaSens == 0)
        return std::nullopt;
    const int64_t mult = kB01.pdiv * kB01.tsampleAte, div = kB01.tsample * kB01.pdivAte;
    int64_t therma = s16(div64S64Precise(deltaTemp * (1 << 13) * mult, deltaSens * div));
    int64_t thermb = s16(div64S64Precise(ft * actualCp - cp * actualFt, deltaSens));
    therma = s16(div64S64Precise(therma * alpha, 1000000));
    thermb = s16(div64S64Precise(thermb * alpha + beta, 1000000));
    return (uint32_t)((therma & 0xffff) << 16 | (thermb & 0xffff));
}

struct Entry {
    std::string fourcc, name;
    int source;
};

std::vector<Entry> sensors()
{
    std::vector<Entry> out;
    for (unsigned i = 0; i < sensors_count(); ++i)
        out.push_back({sensor_fourcc(i), sensor_name(i), sensor_source(i)});
    return out;
}

bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- the contract with thermalmonitord ------------------------------------------------------------------------------

TEST(SwitchThermalSensors, TableIsThePlist)
{
    const std::vector<Entry> table = sensors();
    std::vector<Sensor> pairs;
    for (const Entry &e : table)
        pairs.emplace_back(e.fourcc, e.name);
    EXPECT_EQ(pairs, j120apHidSensors());
    REQUIRE(table.size() < 64);
    EXPECT_EQ((1ull << table.size()) - 1, kExpectedMask);  // requiresWatchDog defaults to true: one bit each
    std::set<std::string> names;
    for (const Entry &e : table)
        names.insert(e.name);
    EXPECT_EQ(names.size(), table.size());  // Product is the key: all distinct
    for (unsigned i = 0; i < table.size(); ++i) {
        uint32_t big = 0;
        for (char c : table[i].fourcc)
            big = big << 8 | (uint8_t)c;
        EXPECT_EQ(sensor_fourcc_value(i), big) << table[i].fourcc;  // LocationID match
    }
}

// J120AP-Info.plist's infoOnly entries: thermalmonitord never reads them, but sets their health bit when the service
// exists. r8e logged "partial sensors: ... current 9249276ba" with every read failing: exactly these bits.
TEST(SwitchThermalSensors, InfoOnlyIsThePlist)
{
    const std::set<std::string> infoOnly = {"TP1d", "TP3d", "TP4d", "TP5d", "TP7d", "TR1d", "TR2d", "TP0j", "TQ0B",
                                            "Th0i", "Th1i", "Th2i", "Th3i", "Tc0i", "Tc1i", "Tc2i", "Tc3i"};
    uint64_t infoBits = 0;
    for (unsigned i = 0; i < sensors_count(); ++i) {
        EXPECT_EQ(sensor_info_only(i), infoOnly.count(sensor_fourcc(i)) ? 1 : 0) << sensor_fourcc(i);
        if (infoOnly.count(sensor_fourcc(i)))
            infoBits |= 1ull << i;
    }
    EXPECT_EQ(infoBits, 0x9249276baull);
    EXPECT_EQ(sensors_read_mask(), kExpectedMask & ~0x9249276baull);
    EXPECT_EQ(__builtin_popcountll(sensors_read_mask()), 21);
}

TEST(SwitchThermalSensors, EveryValueIsTheNearestMeasurement)
{
    const std::regex board("PMU2? (tdev\\d|tjunc)");
    std::set<int> sources;
    for (const Entry &e : sensors()) {
        SCOPED_TRACE(e.fourcc);
        sources.insert(e.source);
        bool acc = false;
        for (int n = 0; n < 4; ++n)
            acc = acc || endsWith(e.name, "ACC Temp Sensor" + std::to_string(n));
        if (acc) {
            EXPECT_EQ(e.source, kSocCpu[e.name.back() - '0']);  // core n's own tsensor
        } else if (e.name.find("PMGR SOC Die Temp Sensor") != std::string::npos) {
            EXPECT_TRUE(kDiePoints.count(e.source)) << e.source;  // on the die, outside the CPU cores
        } else if (e.name == "gas gauge battery" || e.name == "Charger tbat") {
            EXPECT_EQ(e.source, kBattery);  // the pack's thermistor
        } else if (e.name == "PMU tcal") {
            EXPECT_EQ(e.source, kTcal);
        } else if (std::regex_match(e.name, board)) {
            EXPECT_EQ(e.source, kTmp451Pcb);  // board thermistors: the PCB
        } else {
            ADD_FAILURE() << "unmapped " << e.fourcc << " " << e.name;
        }
    }
    // i/a/x of one PMGR/ACC sensor always read the same point.
    std::map<std::string, std::set<int>> groups;
    for (const Entry &e : sensors())
        if (e.fourcc.rfind("Th", 0) == 0 || e.fourcc.rfind("Tc", 0) == 0)
            groups[e.fourcc.substr(0, 3)].insert(e.source);
    for (const auto &[group, points] : groups)
        EXPECT_EQ(points.size(), 1u) << group;
    // Each of the six SOC_THERM sensors and both TMP451 channels feed something.
    for (int s = 0; s < 10; ++s)
        EXPECT_TRUE(sources.count(s)) << "source " << s;
}

TEST(SwitchThermalSensors, CltmUnits)
{
    for (int milli : {45500, 45499, 0, -1250, 37812, 100000, -39999, 149999}) {
        const int fixed = fixed_of(milli);
        EXPECT_NEAR(fixed / 65536.0, milli / 1000.0, 1 / 65536.0) << milli;
        EXPECT_EQ(cltm_centi(fixed), (int)(fixed / 65536.0 * 100)) << milli;  // (int)(float * 100.0)
    }
    EXPECT_EQ(cltm_centi(fixed_of(45500)), 4550);
    // thermalmonitord's window: -40 C < t < 150 C (centi-degrees).
    EXPECT_TRUE(cltm_valid(-3999));
    EXPECT_FALSE(cltm_valid(-4000));
    EXPECT_TRUE(cltm_valid(14999));
    EXPECT_FALSE(cltm_valid(15000));
    EXPECT_TRUE(cltm_valid(0));
    // Whatever the kernel hands over passes it.
    for (int milli = -39999; milli < 150000; milli += 37)
        if (plausible(milli))
            EXPECT_TRUE(cltm_valid(cltm_centi(fixed_of(milli)))) << milli;
    EXPECT_FALSE(plausible(-40000));
    EXPECT_FALSE(plausible(150000));
}

TEST(SwitchThermalSensors, SocthermReadback)
{
    EXPECT_EQ(soctherm_millic(0x2d80), 45500);  // 45 + half
    EXPECT_EQ(soctherm_millic(0x2d00), 45000);
    EXPECT_EQ(soctherm_millic(0x0581), -5500);  // negate bit
    EXPECT_EQ(soctherm_millic(0x0000), 0);
    for (unsigned raw = 0; raw < 0x10000; raw += 7) {
        const int t = (int)((raw & 0xff00) >> 8) * 1000 + ((raw & 0x80) ? 500 : 0);
        EXPECT_EQ(soctherm_millic(raw), (raw & 1) ? -t : t) << raw;
    }
}

TEST(SwitchThermalSensors, Max17050)
{
    EXPECT_EQ(max17050_millic(0x1f80), 31500);
    EXPECT_EQ(max17050_millic(0xff00), -1000);
    EXPECT_EQ(max17050_millic(0x2560), 37375);
    for (int milli = -30000; milli < 80000; milli += 113) {
        const unsigned talrt = max17050_talrt(milli);
        const int hi = (int8_t)(uint8_t)(talrt >> 8);
        const int lo = (int8_t)(uint8_t)(talrt & 0xff);
        EXPECT_LT(lo * 1000, milli);  // the reading never violates its own window
        EXPECT_GT(hi * 1000, milli);
        EXPECT_LE(hi * 1000 - milli, 1500) << milli;  // about a degree either way
        EXPECT_LE(milli - lo * 1000, 1500) << milli;
    }
}

// ---- SOC_THERM ------------------------------------------------------------------------------------------------------

TEST(SwitchThermalSensors, TsensorLayout)
{
    for (unsigned i = 0; i < std::size(kB01Tsensors); ++i) {
        EXPECT_STREQ(tsensor_name(i), kB01Tsensors[i].name);
        EXPECT_EQ(tsensor_base(i), kB01Tsensors[i].base) << kB01Tsensors[i].name;
        EXPECT_EQ(tsensor_fuse(i), kB01Tsensors[i].fuse) << kB01Tsensors[i].name;
    }
    EXPECT_EQ(tsensor_config0(), (uint32_t)(kB01.tall << 8));
    EXPECT_EQ(tsensor_config1(),
              (uint32_t)((kB01.tsample - 1) | kB01.tiddqEn << 15 | kB01.tenCount << 24 | int64_t{1} << 31));
}

TEST(SwitchThermalSensors, FuseCalibrationMatchesL4t)
{
    std::mt19937 random(1952);
    std::vector<std::pair<uint32_t, uint32_t>> cases = {{0x0f8c3c41, 0x01d0e9d3}};
    for (int i = 0; i < 4000; ++i) {
        const uint32_t common = random();
        cases.emplace_back(common, random());
    }
    unsigned checked = 0;
    for (const auto &[common, fuse] : cases) {
        for (unsigned i = 0; i < std::size(kB01Tsensors); ++i) {
            const std::optional<uint32_t> want = l4tCalib(common, fuse, kB01Tsensors[i].alpha, kB01Tsensors[i].beta);
            const uint32_t got = tsensor_calib(common, fuse, i);
            if (!want) {
                EXPECT_EQ(got, 0u);
                continue;
            }
            ASSERT_EQ(got, *want) << std::hex << common << " " << fuse << " " << i;
            ++checked;
        }
    }
    EXPECT_GT(checked, 20000u);
}

// ---- mitigation -----------------------------------------------------------------------------------------------------

TEST(SwitchThermalSensors, CltmCeiling)
{
    std::vector<std::string> keys;
    for (unsigned i = 0; i < 4; ++i)
        keys.emplace_back(cltm_key(i));
    EXPECT_EQ(keys, kCltmKeys);
    EXPECT_EQ(cltm_ceiling(0x10000, 0x10000, 0x10000, 0x10000), 0x10000u);
    EXPECT_EQ(cltm_ceiling(0x10000, 24248, 0x10000, 0x10000), 24248u);
    EXPECT_EQ(cltm_ceiling(30000, 24248, 0, 0x10000), 0u);
    // The governor's DFLL points (918..1785 MHz, SwitchGovernor.h table on speedo 1659) and CLTM's percentages.
    const std::vector<uint32_t> khz = {918000, 1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000};
    const unsigned top = (unsigned)khz.size() - 1;
    // MitigationController scaleToFixedPoint
    const auto fraction = [](uint32_t percent) {
        return percent < 100 ? percent * 65536 / 100 : 0x10000u;
    };
    const auto index = [&](uint32_t percent) {
        return index_at_fraction(khz.data(), (unsigned)khz.size(), fraction(percent));
    };
    EXPECT_EQ(index(101), top);  // no limit
    EXPECT_EQ(index(100), top);
    EXPECT_EQ(index(0), 0u);              // heavy pressure: the lowest point, never off
    EXPECT_EQ(khz[index(70)], 1224000u);  // 1249.5 MHz allowed
    EXPECT_EQ(khz[index(96)], 1683000u);  // 1713.6 MHz allowed
    unsigned last = 0;
    for (uint32_t percent = 0; percent < 102; ++percent) {
        const unsigned i = index(percent);
        const uint64_t limit = ((uint64_t)khz.back() * fraction(percent)) >> 16;
        EXPECT_GE(i, last) << percent;
        EXPECT_TRUE(i == 0 || khz[i] <= limit) << percent;
        EXPECT_TRUE(i == top || khz[i + 1] > limit) << percent;
        last = i;
    }
}

}  // namespace
