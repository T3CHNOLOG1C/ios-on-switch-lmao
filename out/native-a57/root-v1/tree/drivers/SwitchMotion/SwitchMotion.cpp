// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch OLED's 6-axis IMU as iPadOS's accelerometer and gyroscope (CoreMotion, UIAccelerometer), and optionally its
// device-orientation sensor (backboardd's interface rotation; kOrientationPolicy below, off).
//
// Hardware (switchroot/LineageOS nx-platforms/tegra210-nx-sensor.dtsi imu@0 on spi@7000da00, "multi-driver" probing of
// every part below; L4T drivers/iio/imu/st_lsm6dsx and inv_icm42600, LineageOS inv_icm42607x):
//   * One of six parts, told apart by WHO_AM_I: InvenSense ICM-42607-P (0x60), ICM-42607 (0x67) or ICM-40607 (0x38) at
//     0x75, or ST LSM6DS3H (0x69), LSM6DSE (0x6a) or LSM6DSO (0x6c) at 0x0f. This OLED's is an ICM-42607-P (r7p5-r7p8:
//     0x75 = 0x60; 0x0f = 0x80 is its ACCEL_DATA_Z1 while the accelerometer is off). SPI4 chip select 0, mode 3, 8 MHz
//     (../TegraSPI). Supplied from the battery rail (vdd/vddio-supply = battery_reg), so it is always powered and keeps
//     what an earlier OS set: it is reset here.
//   * Three register maps: the ICM-42607's (TDK DS-000451, the ICM-42670-P's, which the ICM-42607-P shares; TDK's
//     inv_imu_driver.c; Linux inv_icm42607), the ICM-40607's (Linux inv_icm42600) and the ST parts' (st_lsm6dsx).
//   * Data-ready on the part's INT2 (ST st,drdy-int-pin = 2; ICM irq-int2) to GPIO PX2, level high, latched until the
//     status is read (tegra210b01-fric-pinmux.dtsi motion_int_px2: pull-down, input).
//   * Mounting, the DT mount matrices from the part's axes to the console's (handheld landscape: +X right, +Y up, +Z out
//     of the screen): ST X = +y, Y = +x, Z = -z (the 2019-2023 DT); ICM X = -y, Y = +x, Z = +z (CTCaer's 2025 DT, the
//     one that added the ICM-42607-P; the 2023 ICM matrix had Z = -z, a reflection, so one of its signs was wrong). The
//     ICM Z sign is not yet confirmed on this console: the first accelerometer sample of a boot is logged once (post).
//   * Full scale ±8 g and ±2000 dps (ST 0.244 mg and 70 mdps per LSB; ICM 4096 LSB/g and 16.4 LSB/dps, big-endian).
// iPadOS 21H461 (locationd, backboardd and the r7n device logs; SwitchHID.h has the HID side):
//   * locationd and in-process CoreMotion (CLIoHidInterface) open the built-in HID services with usage pairs
//     {0xff00, 3} and {0xff00, 9} (CLAccelerometer/CLGyro::openHidDevice, matching {Built-In: true}), set their
//     "ReportInterval" while a client wants data (0 once none does) and read kIOHIDEventTypeAccelerometer (13: fields
//     0xd0000-2 in g, gravity vector; 0xd0003 = 1 is shake and dropped) and kIOHIDEventTypeGyro (20: 0x140000-2, rad/s)
//     as they come, without conversion. SpringBoard's own device motion (wallpaper parallax, 3AxisDynamicGyro at 90 Hz)
//     is such a client; on r7n it retried both services until it gave up ("is not ready").
//   * backboardd's BKOrientationManager matches {0xff00, 3} for UIAccelerometer clients. Its device orientation comes
//     only from CoreMotion's orientation notifier, which on this hardware type (CoreMotion hwType 62) opens the AOP's
//     device-orientation service {0x20, 0x8a} and takes kIOHIDEventTypeOrientation events (field 0xa0004 = 0x64 +
//     UIDeviceOrientation); it logs "fHidDeviceInterface NULL" without the AOP plug-in but takes the events. With that
//     service SpringBoard rotates the interface; without it the device orientation stays unknown and the interface
//     stays landscape, as now.
// Sampling: the part converts only while a client has a nonzero report interval (the services' Report Interval
// feature, SwitchHID.h) or the orientation detector runs. The output data rate is the lowest supported rate at or above
// the fastest request; the gyroscope's data-ready leads when it is on (both then share the rate) and each service is
// decimated to its own interval. Each data-ready interrupt reads status and data (one SPI burst; the ICM-42607's status
// register is apart from its data, so status first, then data), which lets the line drop. A 1 s watchdog, armed only
// while converting, panics if data-ready is raised but never arrives (lost at the GPIO controller or the GIC); a part
// that raises none, or a line that stays high without new data, turns motion off with one log line (giveUp). No polling
// anywhere.
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <os/log.h>
#include <string.h>
#include "../SwitchTouch/SwitchHID.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraSPI/TegraSPI.h"
#include "SwitchMotion.h"

namespace {
constexpr unsigned kBus = 4, kChipSelect = 0;
constexpr uint32_t kWatchdogMS = 1000;
constexpr unsigned kStuckLimit = 64;  // data-ready interrupts in a row with no new data: INT2 is stuck high

// Device orientation (file comment). Off: the service is not published and the interface stays landscape as now.
// LandscapeOnly: face up/down and the two landscapes only (the interface can turn 180 degrees, never to portrait).
// All: every UIDeviceOrientation, so iPadOS may rotate the interface to portrait.
enum class OrientationPolicy { Off, LandscapeOnly, All };
constexpr OrientationPolicy kOrientationPolicy = OrientationPolicy::Off;
constexpr uint32_t kOrientationMilliHz = 25000;  // the detector's accelerometer rate (ST 26 Hz, ICM 25 Hz)
constexpr unsigned kOrientationStable = 10;      // consecutive samples (about 0.4 s) before a new orientation counts
enum : uint8_t { Unknown, Portrait, PortraitUpsideDown, LandscapeLeft, LandscapeRight, FaceUp, FaceDown };

enum Sensor : unsigned { Accel = 0, Gyro = 1 };
enum class Family : uint8_t { ST, ICM40607, ICM42607 };
struct Axis {
    uint8_t index;
    int8_t sign;
};
// An output data rate: its register code and, for the ICM-42607 only, its UI low-pass filter's bandwidth code
// (GYRO_CONFIG1, ACCEL_CONFIG1).
struct Rate {
    uint32_t milliHz;
    uint8_t code, filter;
};
struct Part {
    const char *name;
    Family family;
    uint8_t whoAmI;
};
// The ICM parts first: their WHO_AM_I at 0x75 decides before the ST one at 0x0f, which on an ICM-42607 is
// ACCEL_DATA_Z1 and holds live data if an earlier OS left the accelerometer on. On the ST parts 0x75 is not WHO_AM_I
// (not checked against their datasheets: reserved or a user register). WHO_AM_I 0x67 is the ICM-42607 and also the
// ICM-42670-P (DS-000451): one map.
const Part kParts[] = {
    {"ICM-42607-P", Family::ICM42607, 0x60}, {"ICM-42607", Family::ICM42607, 0x67},
    {"ICM-40607", Family::ICM40607, 0x38},   {"LSM6DS3H", Family::ST, 0x69},
    {"LSM6DSE", Family::ST, 0x6a},           {"LSM6DSO", Family::ST, 0x6c},
};
// Per family: the DT mount matrix (rows: console X, Y, Z), data rates, LSB scale to micro-g and micro-rad/s, and how
// long a sensor's first samples are dropped after it is turned on (ST: DRDY_MASK already holds data-ready until the
// filters settle; ICM-40607: inv_icm42600 ACCEL_STARTUP 20 ms, GYRO_STOP 150 ms; ICM-42607: TDK drops 20 ms of
// accelerometer and 60 ms of gyroscope data after enable, the gyroscope starting within 40 ms).
struct FamilyInfo {
    Axis mount[3];
    Rate rates[5];
    int64_t accelNum, accelDen, gyroNum, gyroDen;
    uint32_t accelSettleMS, gyroSettleMS;
};
const FamilyInfo kST = {
    {{1, +1}, {0, +1}, {2, -1}},
    {{12500, 0x1, 0}, {26000, 0x2, 0}, {52000, 0x3, 0}, {104000, 0x4, 0}, {208000, 0x5, 0}},
    244,
    1,
    122173,
    100,  // 0.244 mg; 70 mdps = 1221.73 urad/s
    20,
    80,
};
const FamilyInfo kICM40607 = {
    {{1, -1}, {0, +1}, {2, +1}},
    {{12500, 11, 0}, {25000, 10, 0}, {50000, 9, 0}, {100000, 8, 0}, {200000, 7, 0}},
    15625,
    64,
    1064225,
    1000,  // 1e6 / 4096 ug; pi / 180 / 16.4 rad = 1064.225 urad/s
    20,
    150,
};
// The ICM-42607's ODR codes are one above the ICM-40607's (GYRO_CONFIG0/ACCEL_CONFIG0: 200 Hz = 8 .. 12.5 Hz = 12), and
// its UI filter is set per rate near a quarter of it (7 = 16 Hz, the lowest; 6 = 25 Hz; 4 = 53 Hz): its reset value,
// 180 Hz, would alias at these rates. Scale as the ICM-40607 (DS-000451: 4096 LSB/g, 16.4 LSB/dps).
const FamilyInfo kICM42607 = {
    {{1, -1}, {0, +1}, {2, +1}},
    {{12500, 12, 7}, {25000, 11, 7}, {50000, 10, 7}, {100000, 9, 6}, {200000, 8, 4}},
    15625,
    64,
    1064225,
    1000,
    20,
    60,
};

// ST LSM6DS3H/DSE/DSO registers (the same addresses and bits in all three, st_lsm6dsx_core.c and the datasheets).
namespace st {
enum : uint8_t {
    FifoCtrl = 0x0a,  // DS3H FIFO_CTRL5 / DSO FIFO_CTRL4: 0 = bypass
    Int1Ctrl = 0x0d,
    Int2Ctrl = 0x0e,
    WhoAmI = 0x0f,
    Ctrl1Xl = 0x10,  // ODR 7:4, FS 3:2
    Ctrl2G = 0x11,   // ODR 7:4, FS 3:2
    Ctrl3C = 0x12,
    Ctrl4C = 0x13,
    Status = 0x1e,  // the burst: STATUS, 0x1f, OUT_TEMP, OUTX_L_G .. OUTZ_H_G, OUTX_L_XL .. OUTZ_H_XL
};
enum : uint8_t {
    SwReset = 1u << 0,
    IfInc = 1u << 2,
    Bdu = 1u << 6,
    DrdyMask = 1u << 3,  // CTRL4_C: no data-ready until the filters settle
    Fs8g = 3u << 2,
    Fs2000dps = 3u << 2,
    DrdyXl = 1u << 0,  // INT2_CTRL and STATUS bit 0, accelerometer
    DrdyG = 1u << 1,   // gyroscope
};
constexpr size_t kBurst = 16;
}  // namespace st

// InvenSense ICM-40607 registers, user bank 0 (inv_icm42600.h).
namespace icm40607 {
enum : uint8_t {
    DeviceConfig = 0x11,
    DriveConfig = 0x13,
    IntConfig = 0x14,
    FifoConfig = 0x16,
    AccelData = 0x1f,  // the burst: ACCEL X1..Z0, GYRO X1..Z0 (big-endian), TMST_FSYNC, INT_STATUS
    IntStatus = 0x2d,
    IntfConfig0 = 0x4c,
    PwrMgmt0 = 0x4e,
    GyroConfig0 = 0x4f,
    AccelConfig0 = 0x50,
    IntConfig1 = 0x64,
    IntSource0 = 0x65,
    IntSource3 = 0x68,
    WhoAmI = 0x75,
};
enum : uint8_t {
    SoftReset = 1u << 0,
    ResetDone = 1u << 4,
    DataReady = 1u << 3,  // INT_STATUS, and UI_DRDY_INT2_EN in INT_SOURCE3
    Int2LatchedPushPullHigh = (1u << 5) | (1u << 4) | (1u << 3),
    AsyncReset = 1u << 4,  // INT_CONFIG1: must be 0 for the INT pins to work
    SpiSlewFastest = 5,    // DRIVE_CONFIG 2:0
    I2CDisable = 3,
    SifsMask = 3,
    BigEndian = 1u << 4,  // INTF_CONFIG0
    TempDisable = 1u << 5,
    GyroLowNoise = 3u << 2,
    AccelLowNoise = 3u << 0,  // PWR_MGMT0
    Fs2000dps = 0u << 5,
    Fs8g = 1u << 5,
};
constexpr size_t kBurst = 15;
}  // namespace icm40607

// InvenSense ICM-42607-P / ICM-42607 registers, user bank 0 (TDK DS-000451 and inv_imu_regmap_rev_a.h; Linux
// inv_icm42607.h). Everything here is in bank 0: the MREG banks (BLK_SEL/MADDR/M_W) hold nothing this driver uses.
namespace icm42607 {
enum : uint8_t {
    DeviceConfig = 0x01,     // SPI_AP_4WIRE bit 2 (reset value 0x04: 4-wire), SPI_MODE bit 0 (0: modes 0 and 3)
    SignalPathReset = 0x02,  // SOFT_RESET_DEVICE_CONFIG bit 4
    DriveConfig3 = 0x05,     // SPI_SLEW_RATE 2:0
    IntConfig = 0x06,
    AccelData = 0x0b,  // the burst: ACCEL X1..Z0, GYRO X1..Z0 (big-endian)
    PwrMgmt0 = 0x1f,
    GyroConfig0 = 0x20,   // FS 6:5, ODR 3:0
    AccelConfig0 = 0x21,  // FS 6:5, ODR 3:0
    GyroConfig1 = 0x23,   // UI filter bandwidth 2:0
    AccelConfig1 = 0x24,  // averaging 6:4 (low-power mode only), UI filter bandwidth 2:0
    IntSource0 = 0x2b,    // INT1 sources (reset value 0x10: RESET_DONE)
    IntSource3 = 0x2d,    // INT2 sources
    IntfConfig0 = 0x35,
    IntfConfig1 = 0x36,
    IntStatusDrdy = 0x39,  // DATA_RDY_INT bit 0, cleared by the read
    IntStatus = 0x3a,      // RESET_DONE_INT bit 4, cleared by the read
    WhoAmI = 0x75,
    BlkSelW = 0x79,
    BlkSelR = 0x7c,
};
enum : uint8_t {
    Spi4Wire = 1u << 2,
    SoftReset = 1u << 4,
    ResetDone = 1u << 4,
    DataReady = 1u << 0,  // INT_STATUS_DRDY
    DrdyInt2 = 1u << 3,   // INT_SOURCE3
    Int2LatchedPushPullHigh = (1u << 5) | (1u << 4) | (1u << 3),
    SpiSlewFastest = 5,  // DRIVE_CONFIG3, its reset value (bits 7:3 reserved, 0)
    I3cSdr = 1u << 3,
    I3cDdr = 1u << 2,
    ClkSelMask = 3,
    ClkSelPll = 1,  // INTF_CONFIG1: PLL when available, else the RC oscillator (the reset value)
    BigEndian = 1u << 4,
    I2CDisable = 3,           // INTF_CONFIG0 1:0, TDK's "shared SPI bus" setting (AN-000352)
    GyroLowNoise = 3u << 2,   // PWR_MGMT0
    AccelLowNoise = 3u << 0,  // PWR_MGMT0
    Fs2000dps = 0u << 5,
    Fs8g = 1u << 5,
    GyroConfig1Upper = 0x30,   // GYRO_CONFIG1 7:3 (reserved), as reset (0x31)
    AccelConfig1Upper = 0x40,  // ACCEL_CONFIG1 7:3, as reset (0x41: 32x averaging, low-power mode only)
};
constexpr size_t kBurst = 12;
}  // namespace icm42607
static_assert((unsigned)icm40607::WhoAmI == (unsigned)icm42607::WhoAmI, "both ICM maps have WHO_AM_I at 0x75");

// Sample decode (host tests: tests/switch_motion_harness.cpp). ST: STATUS, then gyroscope and accelerometer
// little-endian from offset 4 (st::Status burst). ICM: accelerometer then gyroscope, big-endian, from the burst's
// start; a sensor that is off reads -32768.
void decodeST(const uint8_t *b, int16_t accel[3], int16_t gyro[3], bool fresh[2])
{
    fresh[Accel] = b[0] & st::DrdyXl;
    fresh[Gyro] = b[0] & st::DrdyG;
    for (unsigned i = 0; i < 3; ++i) {
        gyro[i] = (int16_t)(b[4 + 2 * i] | b[5 + 2 * i] << 8);
        accel[i] = (int16_t)(b[10 + 2 * i] | b[11 + 2 * i] << 8);
    }
}

void decodeICM(const uint8_t *b, bool dataReady, int16_t accel[3], int16_t gyro[3], bool fresh[2])
{
    constexpr int16_t kInvalid = -32768;
    fresh[Accel] = fresh[Gyro] = dataReady;
    for (unsigned i = 0; i < 3; ++i) {
        accel[i] = (int16_t)(b[2 * i] << 8 | b[1 + 2 * i]);
        gyro[i] = (int16_t)(b[6 + 2 * i] << 8 | b[7 + 2 * i]);
        if (accel[i] == kInvalid) fresh[Accel] = false;
        if (gyro[i] == kInvalid) fresh[Gyro] = false;
    }
}

// iPadOS's device frame from the part's axes (file comment and SwitchHID.h): the console's frame by the DT mount
// matrix, then the portrait panel's, where +X is the console's down and +Y its right (SwitchTouch's landscape (x, y) ->
// portrait (row 1279 - x, column y)). Acceleration is reported as the gravity vector, the negated specific force.
SwitchMotionReport deviceReport(const FamilyInfo &info, Sensor sensor, const int16_t raw[3])
{
    int32_t console[3];
    for (unsigned i = 0; i < 3; ++i) console[i] = info.mount[i].sign * (int32_t)raw[info.mount[i].index];
    const int32_t sign = sensor == Accel ? -1 : 1;
    const int32_t device[3] = {-console[1] * sign, console[0] * sign, console[2] * sign};
    const int64_t num = sensor == Accel ? info.accelNum : info.gyroNum;
    const int64_t den = sensor == Accel ? info.accelDen : info.gyroDen;
    return {1, (int32_t)(device[0] * num / den), (int32_t)(device[1] * num / den), (int32_t)(device[2] * num / den)};
}

uint64_t msToAbs(uint32_t ms)
{
    uint64_t abs;
    nanoseconds_to_absolutetime((uint64_t)ms * 1000000, &abs);
    return abs;
}
}  // namespace

class SwitchMotion : public IOService {
    OSDeclareDefaultStructors(SwitchMotion)

        // One per HID service with a report interval: the feature handler's context.
        struct Channel {
        SwitchMotion *owner;
        Sensor sensor;
    };

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *irq = nullptr;   // PX2, data-ready
    IOInterruptEventSource *kick = nullptr;  // no provider: a report interval changed (feature handler)
    IOTimerEventSource *watchdog = nullptr;
    IOService *hid[2] = {}, *orientationHid = nullptr;
    Channel channels[2] = {};
    const Part *part = nullptr;
    const FamilyInfo *info = nullptr;
    uint32_t intervalUS[2] = {};  // the clients' report intervals, written by the feature handler (atomics)

    // Work loop state.
    bool converting = false, suspended = false, on[2] = {};
    bool broken = false;       // the part misbehaved on its first boots: logged once, motion off for this boot (giveUp)
    bool firstLogged = false;  // the boot's first trusted accelerometer sample was logged (post)
    unsigned rate = 0;         // index into info->rates while converting
    unsigned every[2] = {}, count[2] = {};  // decimation per service
    uint64_t settle[2] = {};                // first sample time each sensor's output is trusted
    unsigned samples = 0, empty = 0;
    int32_t gravity[3] = {};
    uint8_t orientation = Unknown, candidate = Unknown;
    unsigned candidateCount = 0;

    static void featureReceived(void *context, const uint8_t *report, size_t length);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void kicked(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void watchdogFired(OSObject *owner, IOTimerEventSource *sender);
    void readRegisters(uint8_t reg, uint8_t *buffer, size_t length);
    void writeRegister(uint8_t reg, uint8_t value);
    void giveUp();
    bool identify();
    bool reset();
    void configurePart();
    void program(bool accel, bool gyro, unsigned rateIndex);
    void apply();
    void sample();
    void post(Sensor sensor, const int16_t raw[3], uint64_t now);
    void detectOrientation(const int32_t accel[3]);

public:
    bool start(IOService *provider) override;
    void free() override;
    void suspend();
    void resume();
};

OSDefineMetaClassAndStructors(SwitchMotion, IOService)

    void SwitchMotion::readRegisters(uint8_t reg, uint8_t *buffer, size_t length)
{
    uint8_t tx[TEGRA_SPI_MAX_TRANSFER] = {}, rx[TEGRA_SPI_MAX_TRANSFER];
    tx[0] = (uint8_t)(0x80 | reg);
    const int err = tegra_spi_transfer(kBus, kChipSelect, tx, rx, length + 1);
    if (err) panic("SwitchMotion: SPI4 read of %s register %#x failed (%d)", part ? part->name : "IMU", reg, err);
    memcpy(buffer, rx + 1, length);
}

void SwitchMotion::writeRegister(uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {reg, value};
    const int err = tegra_spi_transfer(kBus, kChipSelect, tx, nullptr, sizeof(tx));
    if (err) panic("SwitchMotion: SPI4 write of %s register %#x failed (%d)", part ? part->name : "IMU", reg, err);
}

// Both WHO_AM_I addresses, read only: nothing is written to a part before it is known (as the Linux drivers, which read
// WHO_AM_I first; the ICM-42607's DEVICE_CONFIG resets to 4-wire SPI). The saved r7p5-r7p8 logs read 0x0f = 0x80,
// 0x75 = 0x60, with and without the ICM-40607 bank-1 INTF_CONFIG4 write r10 added here (6238ae2, on the theory that the
// part was a 40607 left in 3-wire mode): it is an ICM-42607-P, where that write landed on reserved 0x76 and MADDR_W,
// and it is gone. An IMU that is missing leaves motion off with one log line; a lost SPI interrupt still panics in the
// transfer.
bool SwitchMotion::identify()
{
    uint8_t icmId = 0, stId = 0;
    readRegisters(icm42607::WhoAmI, &icmId, 1);
    readRegisters(st::WhoAmI, &stId, 1);
    for (const Part &p : kParts) {
        if ((p.family == Family::ST ? stId : icmId) != p.whoAmI) continue;
        part = &p;
        info = p.family == Family::ST ? &kST : p.family == Family::ICM40607 ? &kICM40607 : &kICM42607;
        return true;
    }
    char sck[256];
    tegra_gpio_describe(TEGRA_GPIO(C, 5), sck, sizeof(sck));
    IOLog("SwitchMotion: off: no IMU on SPI4 CS0: WHO_AM_I 0x75 = %#x, 0x0f = %#x (expected ICM-42607-P 0x60, "
          "ICM-42607 0x67, ICM-40607 0x38 at 0x75; LSM6DS3H 0x69, LSM6DSE 0x6a, LSM6DSO 0x6c at 0x0f); SCK pad %s\n",
          icmId, stId, sck);
    return false;
}

// Software reset and its completion. ST and ICM-40607: then the FIFO in bypass (a reset leaves the FIFO and its
// interrupt as they were, st_lsm6dsx_init_device). ICM-42607 (TDK inv_imu_device_reset): both MREG block selects to 0
// first, the reset, 1 ms, the SPI interface restated (4-wire, modes 0 and 3: the reset value), then RESET_DONE; the
// reset leaves its FIFO bypassed (FIFO_CONFIG1 0x01). A reset that does not complete leaves motion off with one line.
bool SwitchMotion::reset()
{
    switch (part->family) {
    case Family::ST: {
        writeRegister(st::Ctrl3C, st::SwReset);
        IOSleep(10);
        uint8_t ctrl3 = 0, who = 0;
        readRegisters(st::Ctrl3C, &ctrl3, 1);
        if (ctrl3 & st::SwReset) {
            readRegisters(st::WhoAmI, &who, 1);
            IOLog("SwitchMotion: off: %s software reset did not complete (CTRL3_C %#x, WHO_AM_I now %#x)\n", part->name,
                  ctrl3, who);
            return false;
        }
        writeRegister(st::FifoCtrl, 0);
        return true;
    }
    case Family::ICM40607: {
        writeRegister(icm40607::DeviceConfig, icm40607::SoftReset);
        IOSleep(2);  // TDK: 1 ms before any further access
        uint8_t status = 0, config = 0, who = 0;
        readRegisters(icm40607::IntStatus, &status, 1);
        if (!(status & icm40607::ResetDone)) {
            readRegisters(icm40607::DeviceConfig, &config, 1);
            readRegisters(icm40607::WhoAmI, &who, 1);
            IOLog("SwitchMotion: off: %s software reset did not complete (INT_STATUS %#x, DEVICE_CONFIG %#x, "
                  "WHO_AM_I now %#x)\n",
                  part->name, status, config, who);
            return false;
        }
        writeRegister(icm40607::FifoConfig, 0);
        return true;
    }
    case Family::ICM42607: {
        writeRegister(icm42607::BlkSelR, 0);
        writeRegister(icm42607::BlkSelW, 0);
        writeRegister(icm42607::SignalPathReset, icm42607::SoftReset);
        IOSleep(2);  // TDK: 1 ms before any further access
        writeRegister(icm42607::DeviceConfig, icm42607::Spi4Wire);
        uint8_t status = 0, config = 0, who = 0;
        readRegisters(icm42607::IntStatus, &status, 1);
        if (!(status & icm42607::ResetDone)) {
            readRegisters(icm42607::DeviceConfig, &config, 1);
            readRegisters(icm42607::WhoAmI, &who, 1);
            IOLog("SwitchMotion: off: %s software reset did not complete (INT_STATUS %#x, DEVICE_CONFIG %#x, "
                  "WHO_AM_I now %#x)\n",
                  part->name, status, config, who);
            return false;
        }
        return true;
    }
    }
    return false;
}

// Interface and interrupt pin setup, everything converting off. After the reset, and again after sleep.
void SwitchMotion::configurePart()
{
    uint8_t value = 0;
    switch (part->family) {
    case Family::ST:
        writeRegister(st::Ctrl3C, st::Bdu | st::IfInc);  // INT pins active high, push-pull (H_LACTIVE = PP_OD = 0)
        writeRegister(st::Ctrl4C, st::DrdyMask);
        writeRegister(st::Int1Ctrl, 0);
        break;
    case Family::ICM40607:
        readRegisters(icm40607::DriveConfig, &value, 1);
        writeRegister(icm40607::DriveConfig, (uint8_t)((value & ~7u) | icm40607::SpiSlewFastest));
        readRegisters(icm40607::IntfConfig0, &value, 1);
        writeRegister(icm40607::IntfConfig0,
                      (uint8_t)((value & ~icm40607::SifsMask) | icm40607::I2CDisable | icm40607::BigEndian));
        writeRegister(icm40607::IntConfig, icm40607::Int2LatchedPushPullHigh);
        readRegisters(icm40607::IntConfig1, &value, 1);
        writeRegister(icm40607::IntConfig1, (uint8_t)(value & ~icm40607::AsyncReset));
        writeRegister(icm40607::IntSource0, 0);
        break;
    case Family::ICM42607:
        // Linux inv_icm42607 spi_bus_setup and setup, TDK configure_serial_interface: I3C off and the PLL clock,
        // the fastest SPI slew, I2C off (INTF_CONFIG0 1:0, reserved in DS-000451 but set by TDK's own driver) and
        // big-endian data (the reset value); INT2 latched, push-pull, active high; nothing on INT1.
        readRegisters(icm42607::IntfConfig1, &value, 1);
        writeRegister(
            icm42607::IntfConfig1,
            (uint8_t)((value & ~(icm42607::I3cSdr | icm42607::I3cDdr | icm42607::ClkSelMask)) | icm42607::ClkSelPll));
        writeRegister(icm42607::DriveConfig3, icm42607::SpiSlewFastest);
        readRegisters(icm42607::IntfConfig0, &value, 1);
        writeRegister(icm42607::IntfConfig0, (uint8_t)(value | icm42607::I2CDisable | icm42607::BigEndian));
        writeRegister(icm42607::IntConfig, icm42607::Int2LatchedPushPullHigh);
        writeRegister(icm42607::IntSource0, 0);
        break;
    }
    program(false, false, 0);
}

// Data rates, full scales and power for both sensors, and the data-ready source on INT2: the gyroscope's when it is
// on, else the accelerometer's (ST); the shared one (ICM). Turning on writes the sensors first and the routing last;
// turning off the reverse, so INT2 is low when nothing converts.
void SwitchMotion::program(bool accel, bool gyro, unsigned rateIndex)
{
    const Rate &r = info->rates[rateIndex];
    const bool any = accel || gyro;
    switch (part->family) {
    case Family::ST: {
        const uint8_t route = gyro ? st::DrdyG : (accel ? st::DrdyXl : 0);
        if (!route) writeRegister(st::Int2Ctrl, 0);
        writeRegister(st::Ctrl1Xl, accel ? (uint8_t)(r.code << 4 | st::Fs8g) : 0);
        writeRegister(st::Ctrl2G, gyro ? (uint8_t)(r.code << 4 | st::Fs2000dps) : 0);
        if (route) writeRegister(st::Int2Ctrl, route);
        break;
    }
    case Family::ICM40607:
        if (!any) writeRegister(icm40607::IntSource3, 0);
        writeRegister(icm40607::GyroConfig0, (uint8_t)(icm40607::Fs2000dps | r.code));
        writeRegister(icm40607::AccelConfig0, (uint8_t)(icm40607::Fs8g | r.code));
        writeRegister(icm40607::PwrMgmt0, (uint8_t)(icm40607::TempDisable | (gyro ? icm40607::GyroLowNoise : 0) |
                                                    (accel ? icm40607::AccelLowNoise : 0)));
        IODelay(200);  // no register writes for 200 us after a sensor leaves off (datasheet)
        if (any) writeRegister(icm40607::IntSource3, icm40607::DataReady);
        break;
    case Family::ICM42607:
        if (!any) writeRegister(icm42607::IntSource3, 0);
        writeRegister(icm42607::GyroConfig0, (uint8_t)(icm42607::Fs2000dps | r.code));
        writeRegister(icm42607::AccelConfig0, (uint8_t)(icm42607::Fs8g | r.code));
        writeRegister(icm42607::GyroConfig1, (uint8_t)(icm42607::GyroConfig1Upper | r.filter));
        writeRegister(icm42607::AccelConfig1, (uint8_t)(icm42607::AccelConfig1Upper | r.filter));
        writeRegister(icm42607::PwrMgmt0,
                      (uint8_t)((gyro ? icm42607::GyroLowNoise : 0) | (accel ? icm42607::AccelLowNoise : 0)));
        IODelay(200);  // no register writes for 200 us after a sensor leaves off (DS-000451 PWR_MGMT0)
        if (any) writeRegister(icm42607::IntSource3, icm42607::DrdyInt2);
        break;
    }
}

// Work loop. A part that does not behave as expected on hardware that has not booted this driver before is not worth the
// boot: one log line (the caller's), the part stopped, motion off until the next boot. A lost interrupt still panics
// (tegra_gpio_check_delivery, TegraSPI).
void SwitchMotion::giveUp()
{
    broken = true;
    if (converting) {
        program(false, false, 0);
        irq->disable();
        watchdog->cancelTimeout();
        converting = false;
    }
    on[Accel] = on[Gyro] = false;
}

// Work loop: the clients' intervals (and the orientation detector) to a rate, sensors, routing and decimation.
void SwitchMotion::apply()
{
    if (suspended || broken) return;
    uint32_t interval[2];
    for (unsigned s = 0; s < 2; ++s) interval[s] = __atomic_load_n(&intervalUS[s], __ATOMIC_ACQUIRE);
    const bool detector = kOrientationPolicy != OrientationPolicy::Off;
    const bool want[2] = {interval[Accel] != 0 || detector, interval[Gyro] != 0};
    uint32_t need = detector ? kOrientationMilliHz : 0;
    for (unsigned s = 0; s < 2; ++s) {
        const uint32_t milliHz = interval[s] ? (uint32_t)((1000000000ull + interval[s] - 1) / interval[s]) : 0;
        if (milliHz > need) need = milliHz;
    }
    unsigned next = 0;
    while (next + 1 < 5 && info->rates[next].milliHz < need) ++next;

    if (!want[Accel] && !want[Gyro]) {
        if (converting) {
            program(false, false, 0);
            irq->disable();
            watchdog->cancelTimeout();
            converting = false;
            os_log(OS_LOG_DEFAULT, "SwitchMotion: off");
        }
        on[Accel] = on[Gyro] = false;
        return;
    }
    if (!converting || next != rate || want[Accel] != on[Accel] || want[Gyro] != on[Gyro]) {
        const uint64_t now = mach_absolute_time();
        for (unsigned s = 0; s < 2; ++s) {
            if (want[s] && !on[s]) settle[s] = now + msToAbs(s == Accel ? info->accelSettleMS : info->gyroSettleMS);
        }
        program(want[Accel], want[Gyro], next);
        on[Accel] = want[Accel];
        on[Gyro] = want[Gyro];
        rate = next;
        if (!converting) {
            converting = true;
            samples = empty = 0;
            irq->enable();
            watchdog->setTimeoutMS(kWatchdogMS);
        }
        // Log only (not the console): one line per change of what the part converts.
        os_log(OS_LOG_DEFAULT, "SwitchMotion: %u mHz, accel %d (%u us), gyro %d (%u us)", info->rates[rate].milliHz,
               on[Accel], interval[Accel], on[Gyro], interval[Gyro]);
    }
    for (unsigned s = 0; s < 2; ++s) {
        const uint64_t perReport = (uint64_t)info->rates[rate].milliHz * interval[s] / 1000000000ull;
        every[s] = perReport > 1 ? (unsigned)perReport : 1;
        count[s] = 0;
    }
}

// One trusted sample to its HID service (deviceReport has the frames). The boot's first accelerometer sample is logged
// once with its raw axes, so one look at the log settles the mount matrix's signs: lying face up, the device frame
// reads z near -1000 mg; held upright in handheld landscape, x near +1000 mg (the portrait panel's +X is the
// console's bottom).
void SwitchMotion::post(Sensor sensor, const int16_t raw[3], uint64_t now)
{
    if (now < settle[sensor]) return;
    const SwitchMotionReport report = deviceReport(*info, sensor, raw);
    if (sensor == Accel && !firstLogged) {
        firstLogged = true;
        IOLog("SwitchMotion: %s first accelerometer sample: raw %d %d %d, device frame %d %d %d mg (face up: z near "
              "-1000; upright: x near +1000)\n",
              part->name, raw[0], raw[1], raw[2], report.x / 1000, report.y / 1000, report.z / 1000);
    }
    if (sensor == Accel && kOrientationPolicy != OrientationPolicy::Off) {
        const int32_t vector[3] = {report.x, report.y, report.z};
        detectOrientation(vector);
    }
    if (!__atomic_load_n(&intervalUS[sensor], __ATOMIC_RELAXED) || ++count[sensor] < every[sensor]) return;
    count[sensor] = 0;
    (void)switch_hid_post(hid[sensor], &report, sizeof(report));  // dropped until the event driver has opened it
}

// The AOP's device orientation from the gravity vector (micro-g, device frame), with the tilt and hysteresis margins
// Apple's documentation describes: face up/down within 25 degrees of flat (leaving it past 35), portrait against
// landscape 10 degrees past the diagonal, nothing while the device is accelerating, and 0.4 s of agreement.
void SwitchMotion::detectOrientation(const int32_t accel[3])
{
    for (unsigned i = 0; i < 3; ++i) gravity[i] += (accel[i] - gravity[i]) / 4;
    const int64_t x2 = (int64_t)gravity[0] * gravity[0], y2 = (int64_t)gravity[1] * gravity[1],
                  z2 = (int64_t)gravity[2] * gravity[2], n2 = x2 + y2 + z2;
    if (n2 < 562500000000ll || n2 > 1562500000000ll) return;  // |g| outside 0.75 .. 1.25 g
    const bool face = orientation == FaceUp || orientation == FaceDown;
    const bool upright = orientation >= Portrait && orientation <= LandscapeRight;
    uint8_t next = orientation;
    if (z2 * 1000 > n2 * (face ? 671 : 821)) {  // cos^2 35 / cos^2 25
        next = gravity[2] < 0 ? FaceUp : FaceDown;
    } else if (y2 * 100 > x2 * 204) {  // tan^2 55
        next = gravity[1] < 0 ? Portrait : PortraitUpsideDown;
    } else if (x2 * 100 > y2 * 204) {
        next = gravity[0] > 0 ? LandscapeRight : LandscapeLeft;
    } else if (!upright) {
        next = x2 > y2 ? (gravity[0] > 0 ? LandscapeRight : LandscapeLeft)
                       : (gravity[1] < 0 ? Portrait : PortraitUpsideDown);
    }
    if (kOrientationPolicy == OrientationPolicy::LandscapeOnly && (next == Portrait || next == PortraitUpsideDown)) {
        next = orientation;
    }
    if (next == orientation) {
        candidateCount = 0;
        return;
    }
    if (next != candidate) {
        candidate = next;
        candidateCount = 0;
    }
    if (++candidateCount < kOrientationStable) return;
    orientation = next;
    candidateCount = 0;
    const SwitchOrientationReport report = {1, (uint8_t)(1u << orientation)};
    (void)switch_hid_post(orientationHid, &report, sizeof(report));
}

// Work loop, PX2 masked until return: status and data, which releases the latched data-ready.
void SwitchMotion::sample()
{
    const uint64_t now = mach_absolute_time();
    int16_t accel[3] = {}, gyro[3] = {};
    bool fresh[2] = {};
    switch (part->family) {
    case Family::ST: {
        uint8_t b[st::kBurst];
        readRegisters(st::Status, b, sizeof(b));
        decodeST(b, accel, gyro, fresh);
        break;
    }
    case Family::ICM40607: {
        uint8_t b[icm40607::kBurst];
        readRegisters(icm40607::AccelData, b, sizeof(b));
        decodeICM(b, b[14] & icm40607::DataReady, accel, gyro, fresh);
        break;
    }
    case Family::ICM42607: {
        // TDK inv_imu_get_data_from_registers: INT_STATUS_DRDY first (the read clears it and drops INT2), then data.
        uint8_t status = 0, b[icm42607::kBurst];
        readRegisters(icm42607::IntStatusDrdy, &status, 1);
        readRegisters(icm42607::AccelData, b, sizeof(b));
        decodeICM(b, status & icm42607::DataReady, accel, gyro, fresh);
        break;
    }
    }
    fresh[Accel] = fresh[Accel] && on[Accel];
    fresh[Gyro] = fresh[Gyro] && on[Gyro];
    if (!fresh[Accel] && !fresh[Gyro]) {
        if (++empty > kStuckLimit) {
            char state[256];
            tegra_gpio_describe(TEGRA_GPIO_IMU_INT, state, sizeof(state));
            IOLog("SwitchMotion: off: %s data-ready (PX2) fired %u times in a row with no new data: %s\n", part->name,
                  empty, state);
            giveUp();
        }
        return;
    }
    empty = 0;
    ++samples;
    if (fresh[Gyro]) post(Gyro, gyro, now);
    if (fresh[Accel]) post(Accel, accel, now);
}

void SwitchMotion::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchMotion, owner);
    if (self && self->converting) self->sample();
}

void SwitchMotion::kicked(OSObject *owner, IOInterruptEventSource *, int)
{
    if (auto *self = OSDynamicCast(SwitchMotion, owner)) self->apply();
}

// Armed only while converting. No sample in a whole period: either the latched line is high and its interrupt never
// reached us (tegra_gpio_check_delivery panics and says where it stopped), or the part stopped producing data-ready.
void SwitchMotion::watchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchMotion, owner);
    if (!self || !self->converting) return;
    if (!self->samples) {
        tegra_gpio_check_delivery(TEGRA_GPIO_IMU_INT, "SwitchMotion");
        const uint32_t milliHz = self->info->rates[self->rate].milliHz;
        IOLog("SwitchMotion: off: %s raised no data-ready on PX2 in %u ms at %u.%u Hz (accel %d, gyro %d): INT2 not "
              "wired or the part stopped\n",
              self->part->name, kWatchdogMS, milliHz / 1000, milliHz % 1000 / 100, self->on[Accel], self->on[Gyro]);
        self->giveUp();
        return;
    }
    self->samples = 0;
    sender->setTimeoutMS(kWatchdogMS);
}

// IOHIDEventDriver's thread, possibly holding its work loop gate (SwitchHID.h): store and kick, never wait.
void SwitchMotion::featureReceived(void *context, const uint8_t *report, size_t length)
{
    auto *channel = static_cast<Channel *>(context);
    if (length < 4) return;
    const uint8_t *value = report + length - 4;  // the Report Interval field ends the report (after the ID, if any)
    const uint32_t interval =
        (uint32_t)value[0] | (uint32_t)value[1] << 8 | (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
    __atomic_store_n(&channel->owner->intervalUS[channel->sensor], interval, __ATOMIC_RELEASE);
    channel->owner->kick->interruptOccurred(nullptr, nullptr, 0);
}

bool SwitchMotion::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    const int err = tegra_spi_init(kBus);
    if (err) panic("SwitchMotion: SPI4 init failed (%d)", err);
    if (!identify() || !reset()) return false;
    configurePart();

    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_IMU_INT, TEGRA_GPIO_LEVEL_HIGH);
    if (!nub) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_IMU_INT, state, sizeof(state));
        panic("SwitchMotion: no interrupt nub for the IMU data-ready line (PX2): %s", state);
    }
    loop = IOWorkLoop::workLoop();
    irq = IOInterruptEventSource::interruptEventSource(this, &SwitchMotion::interruptOccurred, nub, 0);
    kick = IOInterruptEventSource::interruptEventSource(this, &SwitchMotion::kicked, nullptr, 0);
    watchdog = IOTimerEventSource::timerEventSource(this, &SwitchMotion::watchdogFired);
    if (!loop || !irq || !kick || !watchdog || loop->addEventSource(irq) != kIOReturnSuccess ||
        loop->addEventSource(kick) != kIOReturnSuccess || loop->addEventSource(watchdog) != kIOReturnSuccess) {
        panic("SwitchMotion: work loop setup failed");
    }
    irq->disable();  // enabled while converting
    kick->enable();

    const Sensor sensors[] = {Accel, Gyro};
    for (Sensor s : sensors) {
        channels[s] = {this, s};
        hid[s] = switch_hid_create(s == Accel ? kSwitchHIDAccelerometer : kSwitchHIDGyro);
        switch_hid_set_feature_handler(hid[s], &SwitchMotion::featureReceived, &channels[s]);
    }
    if (kOrientationPolicy != OrientationPolicy::Off) orientationHid = switch_hid_create(kSwitchHIDOrientation);
    setProperty("IMU", part->name);
    registerService();
    kick->interruptOccurred(nullptr, nullptr, 0);  // the orientation detector, when it is on
    IOLog("SwitchMotion: started (%s on SPI4, data-ready on PX2)\n", part->name);
    return true;
}

void SwitchMotion::free()
{
    if (loop) {
        if (irq) loop->removeEventSource(irq);
        if (kick) loop->removeEventSource(kick);
        if (watchdog) loop->removeEventSource(watchdog);
    }
    OSSafeReleaseNULL(irq);
    OSSafeReleaseNULL(kick);
    OSSafeReleaseNULL(watchdog);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

// System sleep (SwitchMotion.h): the part off, no interrupt and no watchdog, then the SPI clock off. The clients'
// intervals are kept, so resume converts again exactly as before.
void SwitchMotion::suspend()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchMotion *>(owner);
            if (self->suspended) return kIOReturnSuccess;
            if (self->converting) {
                self->program(false, false, 0);
                self->irq->disable();
                self->watchdog->cancelTimeout();
                self->converting = false;
            }
            self->on[Accel] = self->on[Gyro] = false;
            self->suspended = true;
            return kIOReturnSuccess;
        },
        this);
    tegra_spi_suspend(kBus);
}

void SwitchMotion::resume()
{
    tegra_spi_resume(kBus);
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchMotion *>(owner);
            if (!self->suspended) return kIOReturnSuccess;
            self->suspended = false;
            self->configurePart();  // the part stays powered through SC7; this only restates the pin setup
            self->apply();
            return kIOReturnSuccess;
        },
        this);
}

namespace {
SwitchMotion *volatile gMotion;
}

extern "C" int switch_motion_start(void)
{
    if (gMotion) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchMotion *candidate = OSTypeAlloc(SwitchMotion);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        panic("SwitchMotion: attach failed");
    }
    if (!candidate->start(resources)) {  // no IMU, or it did not come out of reset: logged, motion stays off
        candidate->detach(resources);
        candidate->release();
        return 1;
    }
    __atomic_store_n(&gMotion, candidate, __ATOMIC_RELEASE);
    candidate->release();  // the attachment keeps it
    return 0;
}

extern "C" void switch_motion_suspend(void)
{
    if (SwitchMotion *motion = __atomic_load_n(&gMotion, __ATOMIC_ACQUIRE)) motion->suspend();
}

extern "C" void switch_motion_resume(void)
{
    if (SwitchMotion *motion = __atomic_load_n(&gMotion, __ATOMIC_ACQUIRE)) motion->resume();
}
