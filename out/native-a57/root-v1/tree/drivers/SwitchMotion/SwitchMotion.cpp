// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch OLED's 6-axis IMU as iPadOS's accelerometer and gyroscope (CoreMotion, UIAccelerometer), and optionally its
// device-orientation sensor (backboardd's interface rotation; kOrientationPolicy below, off).
//
// Hardware (switchroot nx-platforms/tegra210-nx-sensor.dtsi imu@0 on spi@7000da00, enabled for the OLED "fric"; L4T
// drivers/iio/imu/st_lsm6dsx and inv_icm42600 with CTCaer's "multi-driver" probing, switch-l4t-kernel-4.9):
//   * One of four parts, told apart by WHO_AM_I: ST LSM6DS3H (0x69), LSM6DSE (0x6a) or LSM6DSO (0x6c) at 0x0f, or
//     InvenSense ICM-40607 (0x38) at 0x75. SPI4 chip select 0, mode 3, 8 MHz (../TegraSPI). Supplied from the battery
//     rail (vdd/vddio-supply = battery_reg), so it is always powered and keeps what an earlier OS set: it is reset here.
//   * Data-ready on the part's INT2 (ST st,drdy-int-pin = 2; ICM irq-int2) to GPIO PX2, level high, latched until the
//     data is read (tegra210b01-fric-pinmux.dtsi motion_int_px2: pull-down, input).
//   * Mounting, the DT mount matrices from the part's axes to the console's (handheld landscape: +X right, +Y up, +Z out
//     of the screen): ST X = +y, Y = +x, Z = -z; ICM X = -y, Y = +x, Z = -z.
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
// decimated to its own interval. Each data-ready interrupt reads status and data in one SPI burst, which lets the line
// drop. A 1 s watchdog, armed only while converting, panics if data-ready stopped arriving (lost at the GPIO controller
// or the GIC, or the part stopped); a line that stays high without new data panics too. No polling anywhere.
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
enum class Family : uint8_t { ST, ICM };
struct Axis {
    uint8_t index;
    int8_t sign;
};
struct Rate {
    uint32_t milliHz;
    uint8_t code;
};
struct Part {
    const char *name;
    Family family;
    uint8_t whoAmI;
};
const Part kParts[] = {
    {"LSM6DS3H", Family::ST, 0x69},
    {"LSM6DSE", Family::ST, 0x6a},
    {"LSM6DSO", Family::ST, 0x6c},
    {"ICM-40607", Family::ICM, 0x38},
};
// Per family: the DT mount matrix (rows: console X, Y, Z), data rates, LSB scale to micro-g and micro-rad/s, and how
// long a sensor's first samples are dropped after it is turned on (ST: DRDY_MASK already holds data-ready until the
// filters settle; ICM: inv_icm42600 ACCEL_STARTUP 20 ms, GYRO_STARTUP 60 ms and GYRO_STOP 150 ms).
struct FamilyInfo {
    Axis mount[3];
    Rate rates[5];
    int64_t accelNum, accelDen, gyroNum, gyroDen;
    uint32_t accelSettleMS, gyroSettleMS;
};
const FamilyInfo kST = {
    {{1, +1}, {0, +1}, {2, -1}},
    {{12500, 0x1}, {26000, 0x2}, {52000, 0x3}, {104000, 0x4}, {208000, 0x5}},
    244,
    1,
    122173,
    100,  // 0.244 mg; 70 mdps = 1221.73 urad/s
    20,
    80,
};
const FamilyInfo kICM = {
    {{1, -1}, {0, +1}, {2, -1}},
    {{12500, 11}, {25000, 10}, {50000, 9}, {100000, 8}, {200000, 7}},
    15625,
    64,
    1064225,
    1000,  // 1e6 / 4096 ug; pi / 180 / 16.4 rad = 1064.225 urad/s
    20,
    150,
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
namespace icm {
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
constexpr int16_t kInvalid = -32768;
}  // namespace icm

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
    unsigned rate = 0;                      // index into info->rates while converting
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

// WHO_AM_I at 0x0f (ST) or 0x75 (ICM); reading the other family's address is harmless. An IMU that is missing or does not
// come out of reset leaves motion off with one log line (r7p: the reset check panicked the board thread at boot); a lost SPI
// interrupt still panics in the transfer.
bool SwitchMotion::identify()
{
    uint8_t stId = 0, icmId = 0;
    readRegisters(st::WhoAmI, &stId, 1);
    readRegisters(icm::WhoAmI, &icmId, 1);
    for (const Part &p : kParts) {
        if ((p.family == Family::ST ? stId : icmId) == p.whoAmI) {
            part = &p;
            info = p.family == Family::ST ? &kST : &kICM;
            return true;
        }
    }
    char sck[256];
    tegra_gpio_describe(TEGRA_GPIO(C, 5), sck, sizeof(sck));
    IOLog(
        "SwitchMotion: off: no IMU on SPI4 CS0: WHO_AM_I 0x0f = %#x, 0x75 = %#x (expected LSM6DS3H 0x69, LSM6DSE 0x6a, "
        "LSM6DSO 0x6c, ICM-40607 0x38); SCK pad %s\n",
        stId, icmId, sck);
    return false;
}

// Software reset, then the FIFO in bypass (a reset leaves the FIFO and its interrupt as they were, st_lsm6dsx_init_device).
bool SwitchMotion::reset()
{
    if (info == &kST) {
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
    } else {
        writeRegister(icm::DeviceConfig, icm::SoftReset);
        IOSleep(2);
        uint8_t status = 0, config = 0, who = 0;
        readRegisters(icm::IntStatus, &status, 1);
        if (!(status & icm::ResetDone)) {
            readRegisters(icm::DeviceConfig, &config, 1);
            readRegisters(icm::WhoAmI, &who, 1);
            IOLog("SwitchMotion: off: %s software reset did not complete (INT_STATUS %#x, DEVICE_CONFIG %#x, WHO_AM_I "
                  "now "
                  "%#x)\n",
                  part->name, status, config, who);
            return false;
        }
        writeRegister(icm::FifoConfig, 0);
    }
    return true;
}

// Interface and interrupt pin setup, everything converting off. After the reset, and again after sleep.
void SwitchMotion::configurePart()
{
    if (info == &kST) {
        writeRegister(st::Ctrl3C, st::Bdu | st::IfInc);  // INT pins active high, push-pull (H_LACTIVE = PP_OD = 0)
        writeRegister(st::Ctrl4C, st::DrdyMask);
        writeRegister(st::Int1Ctrl, 0);
    } else {
        uint8_t value = 0;
        readRegisters(icm::DriveConfig, &value, 1);
        writeRegister(icm::DriveConfig, (uint8_t)((value & ~7u) | icm::SpiSlewFastest));
        readRegisters(icm::IntfConfig0, &value, 1);
        writeRegister(icm::IntfConfig0, (uint8_t)((value & ~icm::SifsMask) | icm::I2CDisable | icm::BigEndian));
        writeRegister(icm::IntConfig, icm::Int2LatchedPushPullHigh);
        readRegisters(icm::IntConfig1, &value, 1);
        writeRegister(icm::IntConfig1, (uint8_t)(value & ~icm::AsyncReset));
        writeRegister(icm::IntSource0, 0);
    }
    program(false, false, 0);
}

// Data rates, full scales and power for both sensors, and the data-ready source on INT2: the gyroscope's when it is
// on, else the accelerometer's (ST); the shared one (ICM). Turning on writes the sensors first and the routing last;
// turning off the reverse, so INT2 is low when nothing converts.
void SwitchMotion::program(bool accel, bool gyro, unsigned rateIndex)
{
    const uint8_t code = info->rates[rateIndex].code;
    if (info == &kST) {
        const uint8_t route = gyro ? st::DrdyG : (accel ? st::DrdyXl : 0);
        if (!route) writeRegister(st::Int2Ctrl, 0);
        writeRegister(st::Ctrl1Xl, accel ? (uint8_t)(code << 4 | st::Fs8g) : 0);
        writeRegister(st::Ctrl2G, gyro ? (uint8_t)(code << 4 | st::Fs2000dps) : 0);
        if (route) writeRegister(st::Int2Ctrl, route);
    } else {
        const bool any = accel || gyro;
        if (!any) writeRegister(icm::IntSource3, 0);
        writeRegister(icm::GyroConfig0, (uint8_t)(icm::Fs2000dps | code));
        writeRegister(icm::AccelConfig0, (uint8_t)(icm::Fs8g | code));
        writeRegister(icm::PwrMgmt0,
                      (uint8_t)(icm::TempDisable | (gyro ? icm::GyroLowNoise : 0) | (accel ? icm::AccelLowNoise : 0)));
        IODelay(200);  // no register writes for 200 us after a sensor leaves off (datasheet)
        if (any) writeRegister(icm::IntSource3, icm::DataReady);
    }
}

// Work loop: the clients' intervals (and the orientation detector) to a rate, sensors, routing and decimation.
void SwitchMotion::apply()
{
    if (suspended) return;
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

// iPadOS's device frame from the part's axes (file comment and SwitchHID.h): the console's frame by the DT mount
// matrix, then the portrait panel's, where +X is the console's down and +Y its right (SwitchTouch's landscape (x, y) ->
// portrait (row 1279 - x, column y)). Acceleration is reported as the gravity vector, the negated specific force.
void SwitchMotion::post(Sensor sensor, const int16_t raw[3], uint64_t now)
{
    if (now < settle[sensor]) return;
    int32_t console[3];
    for (unsigned i = 0; i < 3; ++i) console[i] = info->mount[i].sign * (int32_t)raw[info->mount[i].index];
    const int32_t sign = sensor == Accel ? -1 : 1;
    const int32_t device[3] = {-console[1] * sign, console[0] * sign, console[2] * sign};
    const int64_t num = sensor == Accel ? info->accelNum : info->gyroNum;
    const int64_t den = sensor == Accel ? info->accelDen : info->gyroDen;
    SwitchMotionReport report = {1, (int32_t)(device[0] * num / den), (int32_t)(device[1] * num / den),
                                 (int32_t)(device[2] * num / den)};
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

// Work loop, PX2 masked until return: one burst of status and data, which releases the latched data-ready.
void SwitchMotion::sample()
{
    const uint64_t now = mach_absolute_time();
    int16_t accel[3], gyro[3];
    bool fresh[2];
    if (info == &kST) {
        uint8_t b[st::kBurst];
        readRegisters(st::Status, b, sizeof(b));
        fresh[Accel] = b[0] & st::DrdyXl;
        fresh[Gyro] = b[0] & st::DrdyG;
        for (unsigned i = 0; i < 3; ++i) {
            gyro[i] = (int16_t)(b[4 + 2 * i] | b[5 + 2 * i] << 8);
            accel[i] = (int16_t)(b[10 + 2 * i] | b[11 + 2 * i] << 8);
        }
    } else {
        uint8_t b[icm::kBurst];
        readRegisters(icm::AccelData, b, sizeof(b));
        fresh[Accel] = fresh[Gyro] = b[14] & icm::DataReady;
        for (unsigned i = 0; i < 3; ++i) {
            accel[i] = (int16_t)(b[2 * i] << 8 | b[1 + 2 * i]);
            gyro[i] = (int16_t)(b[6 + 2 * i] << 8 | b[7 + 2 * i]);
        }
        if (accel[0] == icm::kInvalid || accel[1] == icm::kInvalid || accel[2] == icm::kInvalid) fresh[Accel] = false;
        if (gyro[0] == icm::kInvalid || gyro[1] == icm::kInvalid || gyro[2] == icm::kInvalid) fresh[Gyro] = false;
    }
    fresh[Accel] = fresh[Accel] && on[Accel];
    fresh[Gyro] = fresh[Gyro] && on[Gyro];
    if (!fresh[Accel] && !fresh[Gyro]) {
        if (++empty > kStuckLimit) {
            char state[256];
            tegra_gpio_describe(TEGRA_GPIO_IMU_INT, state, sizeof(state));
            panic("SwitchMotion: %s data-ready (PX2) fired %u times in a row with no new data: %s", part->name, empty,
                  state);
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
        panic("SwitchMotion: %s raised no data-ready on PX2 in %u ms at %u.%u Hz (accel %d, gyro %d): INT2 not wired "
              "or the part stopped",
              self->part->name, kWatchdogMS, milliHz / 1000, milliHz % 1000 / 100, self->on[Accel], self->on[Gyro]);
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
