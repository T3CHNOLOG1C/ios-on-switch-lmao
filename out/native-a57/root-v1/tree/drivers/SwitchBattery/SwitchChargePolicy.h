// SPDX-License-Identifier: BSD-2-Clause-Patent
// Charging policy for the Switch OLED (Aula): pure functions and limits, no IOKit, so the drivers that act on them
// (SwitchPD: the BM92T36 USB-PD request; SwitchBattery: the BQ24193 charger and the MAX17050 temperature window) and
// the host test (tests/switch_charge_policy_harness.cpp) share one copy.
//
// Every limit is the stricter of Nintendo's and switchroot's for this board; none is ever exceeded:
//   [AMS-PARAM] Atmosphere libstratosphere/source/powctl/driver/impl/powctl_charger_parameters.board.nintendo_nx.inc,
//               ChargeParametersForBatteryVersion0ForAula (Horizon's table for Aula, battery version 0: temp_min 4,
//               temp_low 17, temp_high 51, temp_max 60 C; awake rules TooLow no charge, Low 4208 mV / 768 mA,
//               Medium 4208 mV / 2048 mA, High 3952 mV / 2048 mA below 4050 mV avg VCELL else no charge, TooHigh no
//               charge; battery compensation 0, voltage clamp 0). Selected by GetChargeParameters() in the .cpp next
//               to it; the levels come from boot_check_battery.cpp BatteryChecker::UpdateCharger (temp < temp_min ...).
//   [AMS-INIT]  Atmosphere powctl_bq24193_driver.cpp Bq24193Driver::InitializeSession: pre-charge 128 mA,
//               termination 128 mA, minimum system voltage 3000 mV, watchdog 0 (off), charging safety timer off;
//               its Encode*/Decode* register encodings, which the ones below copy.
//   [AMS-BOOT]  Atmosphere stratosphere/boot/source/boot_charger_driver.hpp ChargerDriver::Initialize: input current
//               limit 500 mA, Hi-Z off, charge mode, before anything is known about the source.
//   [DTB]       The board's own DTB (bazel/pinned/firmware/oled-sku3.dtb, = switchroot nx-platform
//               tegra210b01-nx-pmic.dtsi, which fric/OLED does not override): bq2419x charger
//               ti,input-voltage-limit-millivolt 4360, ti,sys-input-voltage-limit-millivolt 3000,
//               ti,thermal-regulation-threshold-degc 120, ti,pre-charge/charge-term-current-limit-milliamp 128,
//               ti,charge-voltage-limit-millivolt 4208, ti,fast-charge-current-limit-milliamp 2048,
//               ti,temp-range <3 16 50 59>, ti,charge-current-limit <512 768 2048 2048>,
//               ti,charge-thermal-voltage-limit <4208 4208 4208 3952>, ti,watchdog-timeout <0>, ti,no-otg-watchdog,
//               ti,ir-comp-resister-ohm 1 / -voltage-millivolt 1 (register 0); bm92t rohm,pd-5v/9v/12v/15v-current-
//               limit-ma 2000/2000/1500/1200 ("Absolute max is 2.4A, constrained by BQ24193 ILIM").
//   [L4T-PD]    CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c: bm92t_check_pdo (PDO choice),
//               bm92t_calculate_current_limit (PD_POWER_RESERVE_UA 4.5 W, NON_PD_POWER_RESERVE_UA 2.5 W, the per-voltage
//               caps, the BQ24193 input-limit steps), bm92t_send_rdo; PD_INPUT_CURRENT_LIMIT_MAX_MA 3000, 15 V max.
//   [L4T-CHG]   drivers/power/supply/bq2419x-charger.c: bq2419x_charger_thermal_configure (charging off above
//               BQ2419x_TEMP_H_CHG_DISABLE 50 C and below BQ2419x_TEMP_L_CHG_DISABLE 0 C, "temp <= temp_range[i]"),
//               bq2419x_charger_init / bq2419x_watchdog_disable, bq2419x_configure_input_charging_current (IINLIM
//               raised a step at a time from 500 mA, BQ2419x_CHARGING_CURRENT_STEP_DELAY_US 1000).
//   [TI]        TI BQ24193 datasheet SLUSBG7A: 8.3.1.3 input current limit detection on every plug-in (PSEL/OTG),
//               8.3.4.3 INT and the REG09 double read, 8.4.1 host/default mode and the I2C watchdog, 8.5.1 registers;
//               ILIM caps the input current in hardware.
//
// Temperature zones (battery temperature from the MAX17050, as Horizon's BatteryDriver::GetTemperature): the stricter
// of [AMS-PARAM] and [L4T-CHG] at every temperature.
//   Cold   T < 4 C          no charge      ([AMS-PARAM] TooLow; [L4T-CHG] would allow 512 mA at 0-3 C)
//   Cool   4 <= T < 17 C    768 mA, 4208 mV ([AMS-PARAM] Low; [DTB] 768 mA up to 16 C, 2048 mA from 16 to 17 C)
//   Normal 17 <= T < 51 C   2048 mA, 4208 mV ([AMS-PARAM] Medium; [DTB] 2048 mA)
//   Hot    T >= 51 C        no charge      ([L4T-CHG] off above 50 C; [AMS-PARAM] would allow 3952 mV up to 60 C)
// A zone with more charge current is entered only 1 C inside it (hysteresis toward the safer side only).
// Horizon's Aula sleep-only rule (3072 mA / 4000 mV below 23 % in SleepCharge/ShutdownChargeMain) is not used.
#pragma once
#include <stdint.h>

namespace charge {

// --- Limits ----------------------------------------------------------------------------------------------------------
constexpr unsigned kChargeCurrentMaxMA = 2048;   // [AMS-PARAM] Medium, [DTB] fast-charge-current-limit
constexpr unsigned kChargeVoltageMaxMV = 4208;   // [AMS-PARAM] default_charge_voltage_limit, [DTB] charge-voltage-limit
constexpr unsigned kCoolChargeCurrentMA = 768;   // [AMS-PARAM] Low, [DTB] charge-current-limit[1]
constexpr unsigned kOffChargeCurrentMA = 512;    // programmed while charging is off: the BQ24193's minimum ([TI] REG02)
constexpr unsigned kPreChargeMA = 128;           // [AMS-INIT], [DTB]
constexpr unsigned kTerminationMA = 128;         // [AMS-INIT], [DTB]
constexpr unsigned kSystemMinimumMV = 3000;      // [AMS-INIT], [DTB] sys-input-voltage-limit
constexpr unsigned kInputVoltageLimitMV = 4360;  // [DTB] input-voltage-limit (VINDPM; also the datasheet default)
constexpr unsigned kUnknownSourceMA = 500;       // [AMS-BOOT]; [L4T-CHG] "unknown VBUS source" 500 mA
constexpr unsigned kInputLimitMaxMA = 2000;      // highest PD input limit below: [DTB] 5 V / 9 V caps
constexpr unsigned kPdMaxMillivolts = 15000;     // [L4T-PD]: only 5/9/12/15 V fixed PDOs
constexpr unsigned kPdMaxMilliamps = 3000;       // [L4T-PD] PD_INPUT_CURRENT_LIMIT_MAX_MA
constexpr int kColdBelowC = 4, kCoolBelowC = 17, kHotFromC = 51;  // zones above
constexpr int kHysteresisCentiC = 100;
// A battery temperature outside this range (or a TEMP of exactly 0) at start-up is taken as a gauge that does not
// measure: the charger is then left as found (read only) instead of being run on it.
constexpr int kPlausibleMinCentiC = -2000, kPlausibleMaxCentiC = 8000;

// BQ24193 input current limit steps (REG00 IINLIM, [TI] Table 7; [AMS-INIT] DecodeInputCurrentLimit).
constexpr unsigned kInputSteps[8] = {100, 150, 500, 900, 1200, 1500, 2000, 3000};

// --- BQ24193 registers ([TI] 8.5.1) -----------------------------------------------------------------------------------
namespace bq {
enum : uint8_t {
    InputSource = 0x00,
    PowerOnConfig = 0x01,
    ChargeCurrent = 0x02,
    PreChargeTermination = 0x03,
    ChargeVoltage = 0x04,
    TerminationTimer = 0x05,
    IrThermal = 0x06,
    Misc = 0x07,
    SystemStatus = 0x08,
    Fault = 0x09,
    VendorPart = 0x0a,
};
constexpr uint8_t kHiZ = 0x80, kVindpmMask = 0x78, kIinlimMask = 0x07;  // REG00
constexpr uint8_t kChgConfigMask = 0x30, kChgConfigOff = 0x00, kChgConfigCharge = 0x10, kChgConfigOtg = 0x20,
                  kSysMinMask = 0x0e, kBoostLim1300 = 0x01, kWatchdogReset = 0x40;             // REG01
constexpr uint8_t kIchgMask = 0xfc, kForce20Pct = 0x01;                                        // REG02
constexpr uint8_t kVregMask = 0xfc;                                                            // REG04
constexpr uint8_t kEnTerm = 0x80, kWatchdogMask = 0x30, kEnTimer = 0x08;                       // REG05
constexpr uint8_t kBatCompMask = 0xe0, kVclampMask = 0x1c, kTregMask = 0x03, kTreg120 = 0x03;  // REG06
constexpr uint8_t kPowerGood = 0x04, kDpm = 0x08, kThermalReg = 0x02;                          // REG08
constexpr uint8_t kFaultWatchdog = 0x80;                                                       // REG09: default mode

// REG08 VBUS_STAT: 0 none, 1 USB host, 2 adapter, 3 OTG; CHRG_STAT: 0 off, 1 pre-charge, 2 fast charge, 3 done.
inline constexpr unsigned vbusStat(uint8_t reg08)
{
    return reg08 >> 6;
}
inline constexpr unsigned chargeStat(uint8_t reg08)
{
    return (reg08 >> 4) & 3;
}

// [AMS-INIT] EncodeInputCurrentLimit: the highest step at or below ma (100 mA below the first).
inline constexpr uint8_t encodeInputLimit(unsigned ma)
{
    uint8_t step = 0;
    while (step < 7 && kInputSteps[step + 1] <= ma) ++step;
    return step;
}
inline constexpr unsigned decodeInputLimit(uint8_t reg)
{
    return kInputSteps[reg & kIinlimMask];
}

// [AMS-INIT] EncodeFastChargeCurrentLimit (512..4544 mA, 64 mA steps, rounded down), capped at our maximum.
inline constexpr uint8_t encodeChargeCurrent(unsigned ma)
{
    ma = ma > kChargeCurrentMaxMA ? kChargeCurrentMaxMA : ma < 512 ? 512 : ma;
    return (uint8_t)(((ma - 512) >> 6) << 2);
}
inline constexpr unsigned decodeChargeCurrent(uint8_t reg)
{
    return 512 + ((unsigned)(reg & kIchgMask) << 4);
}

// [AMS-INIT] EncodeChargeVoltageLimit (3504..4400 mV, 16 mV steps, rounded down), capped at our maximum.
inline constexpr uint8_t encodeChargeVoltage(unsigned mv)
{
    mv = mv > kChargeVoltageMaxMV ? kChargeVoltageMaxMV : mv < 3504 ? 3504 : mv;
    return (uint8_t)(((mv - 3504) >> 4) << 2);
}
inline constexpr unsigned decodeChargeVoltage(uint8_t reg)
{
    return 3504 + ((unsigned)(reg & kVregMask) << 2);
}

// [AMS-INIT] EncodeInputVoltageLimit (VINDPM 3880..5080 mV, 80 mV steps).
inline constexpr uint8_t encodeInputVoltage(unsigned mv)
{
    mv = mv > 5080 ? 5080 : mv < 3880 ? 3880 : mv;
    return (uint8_t)((((mv - 3880) / 80) & 0xf) << 3);
}
inline constexpr unsigned decodeInputVoltage(uint8_t reg)
{
    return 3880 + ((reg & kVindpmMask) >> 3) * 80;
}

// [AMS-INIT] EncodePreChargeCurrentLimit / EncodeTerminationCurrentLimit (128..2048 mA, 128 mA steps).
inline constexpr uint8_t encodePreChargeTermination(unsigned preMA, unsigned termMA)
{
    preMA = preMA < 128 ? 128 : preMA > 2048 ? 2048 : preMA;
    termMA = termMA < 128 ? 128 : termMA > 2048 ? 2048 : termMA;
    return (uint8_t)(((preMA - 128) >> 7) << 4 | ((termMA - 128) >> 7));
}

// [AMS-INIT] EncodeMinimumSystemVoltageLimit (3000..3700 mV, 100 mV steps) into REG01 bits 3:1.
inline constexpr uint8_t encodeSystemMinimum(unsigned mv)
{
    mv = mv < 3000 ? 3000 : mv > 3700 ? 3700 : mv;
    return (uint8_t)(((mv - 3000) / 100) << 1);
}

inline constexpr unsigned watchdogSeconds(uint8_t reg05)
{
    return ((reg05 & kWatchdogMask) >> 4) == 0 ? 0 : 40u << (((reg05 & kWatchdogMask) >> 4) - 1);
}
}  // namespace bq

// What the charger is set to while we manage it (REG00 VINDPM/Hi-Z, REG02/04 by zone, REG03, REG05, REG06), and the
// masks of the bits we own; the rest stays as the chip has it (the safety-timer length, JEITA_ISET, REG07). EN_TERM is
// ours and on: the chip keeps its registers across reboots, and switchroot's bq2419x_charger_init clears it (Linux
// ends the charge from the gauge instead), so a boot after Linux would otherwise never terminate; Horizon leaves the
// datasheet default (on) with a 128 mA termination current ([AMS-INIT]).
struct ChargerSetting {
    uint8_t reg02, reg03, reg04, reg05, reg06;
};
constexpr uint8_t kReg03Value = bq::encodePreChargeTermination(kPreChargeMA, kTerminationMA);
constexpr uint8_t kReg05Mask = bq::kEnTerm | bq::kWatchdogMask | bq::kEnTimer;
constexpr uint8_t kReg05Value = bq::kEnTerm;  // termination on, watchdog off, safety timer off
constexpr uint8_t kReg06Mask = bq::kBatCompMask | bq::kVclampMask | bq::kTregMask, kReg06Value = bq::kTreg120;
constexpr uint8_t kReg00Vindpm = bq::encodeInputVoltage(kInputVoltageLimitMV);
constexpr uint8_t kReg01SysMin = bq::encodeSystemMinimum(kSystemMinimumMV);

// --- Battery temperature zones -------------------------------------------------------------------------------------
enum class Zone : uint8_t { Cold, Cool, Normal, Hot };

inline constexpr const char *zoneName(Zone z)
{
    return z == Zone::Cold ? "cold" : z == Zone::Cool ? "cool" : z == Zone::Normal ? "normal" : "hot";
}
inline constexpr bool zoneCharges(Zone z)
{
    return z == Zone::Cool || z == Zone::Normal;
}
inline constexpr unsigned zoneChargeCurrent(Zone z)
{
    return z == Zone::Normal ? kChargeCurrentMaxMA : z == Zone::Cool ? kCoolChargeCurrentMA : kOffChargeCurrentMA;
}
inline constexpr unsigned zoneChargeVoltage(Zone)
{
    return kChargeVoltageMaxMV;
}
inline constexpr unsigned zoneRank(Zone z)
{
    return z == Zone::Normal ? 2 : z == Zone::Cool ? 1 : 0;
}

// The zone of a temperature (centi-degrees C) without hysteresis.
inline constexpr Zone rawZone(int centiC)
{
    return centiC < kColdBelowC * 100   ? Zone::Cold
           : centiC < kCoolBelowC * 100 ? Zone::Cool
           : centiC < kHotFromC * 100   ? Zone::Normal
                                        : Zone::Hot;
}

// Next zone from the current one: toward less charge current at once; toward more only when the temperature is
// kHysteresisCentiC inside the new zone on both sides (the least-current zone within +-1 C).
inline constexpr Zone nextZone(Zone current, int centiC)
{
    const Zone raw = rawZone(centiC);
    if (zoneRank(raw) <= zoneRank(current)) return raw;
    const Zone below = rawZone(centiC - kHysteresisCentiC), above = rawZone(centiC + kHysteresisCentiC);
    Zone safe = raw;
    if (zoneRank(below) < zoneRank(safe)) safe = below;
    if (zoneRank(above) < zoneRank(safe)) safe = above;
    return zoneRank(safe) > zoneRank(current) ? safe : current;
}

inline constexpr ChargerSetting chargerSetting(Zone z)
{
    return ChargerSetting{bq::encodeChargeCurrent(zoneChargeCurrent(z)), kReg03Value,
                          bq::encodeChargeVoltage(zoneChargeVoltage(z)), kReg05Value, kReg06Value};
}

// The MAX17050 TALRT_Th (0x02) window that holds while the zone stays: high byte TMAX, low byte TMIN, signed degrees C
// (Linux max17042_battery.c POWER_SUPPLY_PROP_TEMP_ALERT_MIN/MAX; 0x7f80 = off). The gauge alerts when TEMP leaves
// [TMIN, TMAX]; the reading that follows decides with nextZone(). A temperature already outside its zone's window (only
// after a jump across two zones) gets a 1 C window around itself.
inline constexpr uint16_t temperatureWindow(Zone z, int centiC)
{
    int lo = -128, hi = 127;
    switch (z) {
    case Zone::Cold: hi = kColdBelowC + 1; break;
    case Zone::Cool: lo = kColdBelowC, hi = kCoolBelowC + 1; break;
    case Zone::Normal: lo = kCoolBelowC, hi = kHotFromC; break;
    case Zone::Hot: lo = kHotFromC - 1; break;
    }
    const int whole = centiC >= 0 ? centiC / 100 : -((-centiC + 99) / 100);
    if (whole < lo || whole >= hi) lo = whole - 1, hi = whole + 2;
    if (lo < -128) lo = -128;
    if (hi > 127) hi = 127;
    return (uint16_t)((uint8_t)(int8_t)hi << 8 | (uint8_t)(int8_t)lo);
}

// Two TALRT_Th windows at once (the narrower edges of each): for a gauge alert that serves another reader too, e.g. a
// window of about a degree around the reading for thermalmonitord's battery sensors (r15-thermal's
// switch_max17050_talrt). Both windows hold the current reading, so their intersection does; if they do not overlap,
// the zone window `zone` is kept.
inline constexpr uint16_t intersectWindows(uint16_t zone, uint16_t other)
{
    const int zoneHi = (int8_t)(zone >> 8), zoneLo = (int8_t)(zone & 0xff);
    const int otherHi = (int8_t)(other >> 8), otherLo = (int8_t)(other & 0xff);
    const int hi = zoneHi < otherHi ? zoneHi : otherHi, lo = zoneLo > otherLo ? zoneLo : otherLo;
    if (lo > hi) return zone;
    return (uint16_t)((uint8_t)(int8_t)hi << 8 | (uint8_t)(int8_t)lo);
}

// --- USB-PD (BM92T36 READ_PDOS_SRC / CURRENT_PDO / CURRENT_RDO / SET_RDO; USB PD R3.0 6.4.1.2.2 and 6.4.2) -----------
// PDO type (0 fixed, 1 battery, 2 variable, 3 augmented), current (10 mA) and voltage (50 mV) of a fixed PDO.
inline constexpr unsigned pdoType(uint32_t p)
{
    return p >> 30;
}
inline constexpr unsigned pdoMilliamps(uint32_t p)
{
    return (p & 0x3ff) * 10;
}
inline constexpr unsigned pdoMillivolts(uint32_t p)
{
    return ((p >> 10) & 0x3ff) * 50;
}
inline constexpr unsigned rdoPosition(uint32_t r)
{
    return (r >> 28) & 7;
}
inline constexpr unsigned rdoOperatingMilliamps(uint32_t r)
{
    return ((r >> 10) & 0x3ff) * 10;
}
inline constexpr unsigned rdoMaxMilliamps(uint32_t r)
{
    return (r & 0x3ff) * 10;
}
constexpr uint32_t kRdoNoUsbSuspend = 1u << 24, kRdoUsbCommunications = 1u << 25, kRdoMismatch = 1u << 26;

// [L4T-PD] bm92t_calculate_current_limit: keep a USB port's power for the source (4.5 W above 5 V, 2.5 W at 5 V),
// cap per voltage ([DTB] rohm,pd-*-current-limit-ma), then the BQ24193 step at or below. Unlike bm92txx, a PDO with
// less current than the reserve gives 0 (bm92txx's unsigned subtraction wrapped to the cap), and 0 means "no usable
// contract" (below the BQ24193's lowest step).
inline constexpr unsigned pdInputLimit(unsigned millivolts, unsigned milliamps)
{
    if (!millivolts) return 0;
    const unsigned reserve = (millivolts > 5000 ? 4500000u : 2500000u) / millivolts;
    unsigned limit = milliamps > reserve ? milliamps - reserve : 0;
    const unsigned cap = millivolts == 5000 ? 2000 : millivolts == 9000 ? 2000 : millivolts == 12000 ? 1500 : 1200;
    if (limit > cap) limit = cap;
    if (limit < kInputSteps[0]) return 0;
    return kInputSteps[bq::encodeInputLimit(limit)];
}

struct PdChoice {
    unsigned position;  // 1-based object position, 0 = none
    uint32_t pdo;
    unsigned millivolts, milliamps;  // the PDO's
    unsigned inputLimit;             // operating current requested = BQ24193 input limit
    bool dock;                       // the Nintendo dock's two-PDO signature
    const char *refusal;             // why none, when position is 0
};

// [L4T-PD] bm92t_check_pdo: the Nintendo dock signature (exactly 2 PDOs, the first 5 V 500 mA) takes only a fixed 15 V
// 2.6-3 A second PDO; otherwise a fixed PDO at 5/9/12/15 V and <= 3 A, the higher voltage on a tie. bm92txx ranks by the
// PDO's own power; here by the power the charger will take from it (voltage x pdInputLimit), which is what charges:
// for 9 V 2 A and 12 V 1.5 A (both 18 W) bm92txx takes 12 V at 900 mA (10.8 W), this 9 V at 1500 mA (13.5 W). A PDO
// whose input limit is below the 500 mA the charger gets without a contract (kUnknownSourceMA) is never chosen.
inline constexpr PdChoice choosePdo(const uint32_t *pdos, unsigned count)
{
    PdChoice choice{0, 0, 0, 0, 0, false, "no source PDOs (not USB-PD)"};
    if (count > 7) count = 7;
    if (!count) return choice;
    if (count == 2 && pdoMillivolts(pdos[0]) == 5000 && pdoMilliamps(pdos[0]) == 500) {
        const uint32_t p = pdos[1];
        if (pdoType(p) == 0 && pdoMillivolts(p) == 15000 && pdoMilliamps(p) >= 2600 && pdoMilliamps(p) <= 3000) {
            return PdChoice{2, p, 15000, pdoMilliamps(p), pdInputLimit(15000, pdoMilliamps(p)), true, nullptr};
        }
        choice.refusal = "dock signature without 15 V 2.6-3 A";
        return choice;
    }
    unsigned bestWatts = 0;
    choice.refusal = "no fixed 5/9/12/15 V PDO up to 3 A that gives 500 mA or more";
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t p = pdos[i];
        const unsigned mv = pdoMillivolts(p), ma = pdoMilliamps(p);
        if (pdoType(p) != 0 || ma > kPdMaxMilliamps) continue;
        if (mv != 5000 && mv != 9000 && mv != 12000 && mv != kPdMaxMillivolts) continue;
        const unsigned limit = pdInputLimit(mv, ma);
        if (limit < kUnknownSourceMA) continue;
        const unsigned watts = mv / 10 * limit;  // 10 mW units: fits 32 bits
        if (!choice.position || watts > bestWatts || (watts == bestWatts && mv > choice.millivolts)) {
            bestWatts = watts;
            choice = PdChoice{i + 1, p, mv, ma, limit, false, nullptr};
        }
    }
    return choice;
}

// The request for a choice (USB PD R3.0 Table 6-21, fixed RDO): object position, operating current = the input limit,
// maximum operating current = the PDO's current (bm92t_send_rdo: rdo.max_amp = pdo.amp; the BQ24193 draws what its own
// detection allowed until the input limit is written after the contract, so the source budgets the PDO's current),
// USB communications capable and no USB suspend (we are a USB device that keeps charging while a host suspends the
// bus), no capability mismatch, no give-back. bm92txx's rd_object puts usb_comms on bit 26, which the specification
// names Capability Mismatch; this follows the specification (if a source refuses the request, bm92txx's layout is
// the first thing to try).
inline constexpr uint32_t encodeRdo(const PdChoice &c)
{
    return (uint32_t)(c.position & 7) << 28 | kRdoUsbCommunications | kRdoNoUsbSuspend |
           (uint32_t)(c.inputLimit / 10) << 10 | (uint32_t)(c.milliamps / 10);
}

// A contract matches the request when the controller's CURRENT_RDO is ours and CURRENT_PDO is the PDO we chose.
inline constexpr bool contractMatches(const PdChoice &c, uint32_t currentPdo, uint32_t currentRdo)
{
    return c.position && rdoPosition(currentRdo) == c.position && rdoOperatingMilliamps(currentRdo) == c.inputLimit &&
           rdoMaxMilliamps(currentRdo) == c.milliamps && pdoType(currentPdo) == 0 &&
           pdoMillivolts(currentPdo) == c.millivolts && pdoMilliamps(currentPdo) == c.milliamps;
}

// --- Compile-time checks of the limits and encodings -------------------------------------------------------------------
static_assert(bq::decodeChargeCurrent(bq::encodeChargeCurrent(2048)) == 2048, "ICHG 2048");
static_assert(bq::decodeChargeCurrent(bq::encodeChargeCurrent(768)) == 768, "ICHG 768");
static_assert(bq::decodeChargeCurrent(bq::encodeChargeCurrent(4544)) == kChargeCurrentMaxMA, "ICHG capped");
static_assert(bq::decodeChargeVoltage(bq::encodeChargeVoltage(4208)) == 4208, "VREG 4208");
static_assert(bq::decodeChargeVoltage(bq::encodeChargeVoltage(4400)) == kChargeVoltageMaxMV, "VREG capped");
static_assert(bq::decodeInputVoltage(kReg00Vindpm) == 4360 && kReg00Vindpm == 0x30, "VINDPM 4360");
static_assert(kReg03Value == 0x00 && kReg01SysMin == 0x00, "128/128 mA, 3000 mV");
static_assert(kReg05Value == 0x80 && kReg05Mask == 0xb8, "termination on, watchdog off, safety timer off");
static_assert(pdInputLimit(15000, 2600) == 1200 && pdInputLimit(5000, 3000) == 2000, "PD caps");
static_assert(pdInputLimit(5000, 300) == 0, "no wrap below the reserve");
static_assert(chargerSetting(Zone::Normal).reg02 == 0x60 && chargerSetting(Zone::Normal).reg04 == 0xb0, "normal");

}  // namespace charge
