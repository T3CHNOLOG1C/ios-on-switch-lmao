// Host test of drivers/SwitchMotion/SwitchMotion.cpp (test_switch_motion.py compiles and runs this): the real driver over
// tests/switch_motion_stubs.h and register-level models of its IMUs behind tegra_spi_transfer. Each model keeps a
// register file with its datasheet reset values, applies reads' and writes' side effects (soft reset, read-to-clear
// status, latched INT2) and its timing rules, and records every SPI transfer, so each scenario checks the exact
// register traffic, the HID reports (decode, mount matrix, device frame, scale) and the log lines.
//   * ICM-42607-P / ICM-42607: TDK DS-000451 (the ICM-42670-P's map, which they share) reset values; a read or write
//     of an address the datasheet does not list, a write while it is still resetting (1 ms) or within 200 us of a sensor
//     leaving off, or a DEVICE_CONFIG without 4-wire SPI fails the test.
//   * ICM-40607 and LSM6DS3H: enough of their maps for identification, reset, set-up and one sample each (their reset
//     values as the driver's read-modify-writes expect them: inv_icm42600 and st_lsm6dsx).
#include <cinttypes>
#include "../out/native-a57/root-v1/tree/drivers/SwitchMotion/SwitchMotion.cpp"

static unsigned gChecks = 0, gFailures = 0;
static void check(bool ok, const std::string &what)
{
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}
static std::string hex(unsigned v)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%02x", v);
    return text;
}

// --- IMU models -------------------------------------------------------------------------------------------------------
struct Chip {
    std::vector<std::string> ops;     // "R 75", "R 0b x12", "W 02 10"
    std::vector<std::string> errors;  // broken datasheet rules
    virtual ~Chip() = default;
    virtual void access() {}
    virtual uint8_t read(uint8_t reg) = 0;  // one byte of a read, with its side effects
    virtual void write(uint8_t reg, uint8_t value) = 0;
    virtual bool line() const = 0;  // INT2, which drives PX2
    virtual void produce(const int16_t accel[3], const int16_t gyro[3]) = 0;  // the next output data rate tick
    void fail(const std::string &why) { errors.push_back(why); }
};
static Chip *gChip;

static const int16_t kNoData[3] = {-32768, -32768, -32768};  // what a sensor that is off reads
static void storeBigEndian(uint8_t *r, const int16_t v[3])
{
    for (unsigned i = 0; i < 3; ++i) {
        r[2 * i] = (uint8_t)((uint16_t)v[i] >> 8);
        r[2 * i + 1] = (uint8_t)v[i];
    }
}

struct Icm42607 : Chip {
    uint8_t r[0x80] = {};
    const uint8_t who;
    bool resetBroken = false;  // the soft reset never reports RESET_DONE
    bool stuck = false;        // INT2 held high with no data-ready status
    uint64_t accessFrom = 0, writesFrom = 0;
    explicit Icm42607(uint8_t whoAmI = 0x60) : who(whoAmI) { resetValues(); }
    void resetValues()
    {
        memset(r, 0, sizeof(r));
        const uint8_t reset[][2] = {{0x00, 0x08}, {0x01, 0x04}, {0x03, 0x2b}, {0x04, 0x0d}, {0x05, 0x05}, {0x09, 0x80},
                                    {0x20, 0x06}, {0x21, 0x06}, {0x23, 0x31}, {0x24, 0x41}, {0x25, 0x08}, {0x26, 0x02},
                                    {0x28, 0x01}, {0x2b, 0x10}, {0x34, 0x04}, {0x35, 0x30}, {0x36, 0x4d}, {0x3a, 0x10},
                                    {0x3f, 0xff}};
        for (const auto &v : reset) r[v[0]] = v[1];
        for (uint8_t a = 0x0b; a <= 0x16; a += 2) r[a] = 0x80;  // -32768: no data
        r[0x75] = who;
    }
    static bool writable(uint8_t reg)
    {
        return (reg >= 0x01 && reg <= 0x06) || (reg >= 0x1f && reg <= 0x2e) || reg == 0x35 || reg == 0x36 ||
               (reg >= 0x79 && reg <= 0x7e);
    }
    static bool listed(uint8_t reg)
    {
        return writable(reg) || reg == 0x00 || (reg >= 0x09 && reg <= 0x18) || reg == 0x1d || reg == 0x1e ||
               (reg >= 0x2f && reg <= 0x34) || (reg >= 0x39 && reg <= 0x3f) || reg == 0x75;
    }
    void access() override
    {
        if (gNow < accessFrom) fail("access within 1 ms of the soft reset");
    }
    uint8_t read(uint8_t reg) override
    {
        if (reg >= 0x80 || !listed(reg)) {
            fail("read of unlisted register " + hex(reg));
            return 0;
        }
        const uint8_t value = r[reg];
        if (reg >= 0x39 && reg <= 0x3c) r[reg] = 0;  // INT_STATUS_DRDY .. INT_STATUS3: cleared by the read
        return value;
    }
    void write(uint8_t reg, uint8_t value) override
    {
        if (reg >= 0x80 || !writable(reg)) return fail("write of " + hex(value) + " to read-only/unlisted " + hex(reg));
        if (gNow < writesFrom) fail("write to " + hex(reg) + " within 200 us of a sensor leaving off");
        switch (reg) {
        case 0x01:
            if ((value & 0x05) != 0x04) fail("DEVICE_CONFIG " + hex(value) + ": not 4-wire SPI mode 0/3");
            r[reg] = value & 0x05;
            return;
        case 0x02:
            if (value & 0x10) {
                resetValues();
                if (resetBroken) r[0x3a] = 0;
                accessFrom = gNow + 1000000;
            }
            return;
        case 0x1f: {
            const uint8_t old = r[0x1f];
            if ((value & 0x0c) == 0x08 || (value & 0x60)) fail("PWR_MGMT0 " + hex(value) + ": reserved bits/mode");
            if (((old & 0x0c) == 0 && (value & 0x0c)) || ((old & 0x03) < 2 && (value & 0x03) >= 2)) {
                writesFrom = gNow + 200000;
            }
            if ((value & 0x03) < 2) storeBigEndian(r + 0x0b, kNoData);
            if ((value & 0x0c) != 0x0c) storeBigEndian(r + 0x11, kNoData);
            r[reg] = value;
            return;
        }
        default:
            r[reg] = value;
        }
    }
    bool line() const override { return stuck || ((r[0x2d] & 0x08) && (r[0x39] & 1) && (r[0x06] & 0x38) == 0x38); }
    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        const bool accelOn = (r[0x1f] & 0x03) == 0x03, gyroOn = (r[0x1f] & 0x0c) == 0x0c;
        if (!accelOn && !gyroOn) return fail("a sample with both sensors off");
        for (uint8_t config : {(uint8_t)0x20, (uint8_t)0x21}) {
            const unsigned code = r[config] & 0x0f;
            if (code < 5 || code > 12) fail("ODR code " + hex(code) + " in " + hex(config) + " (low-noise: 5..12)");
        }
        if (accelOn) storeBigEndian(r + 0x0b, accel);
        if (gyroOn) storeBigEndian(r + 0x11, gyro);
        r[0x39] |= 1;
    }
};

struct Icm40607 : Chip {
    uint8_t r[0x80] = {};
    Icm40607() { resetValues(); }
    void resetValues()
    {
        memset(r, 0, sizeof(r));
        const uint8_t reset[][2] = {{0x13, 0x05}, {0x2d, 0x10}, {0x4c, 0x30}, {0x4f, 0x06}, {0x50, 0x06},
                                    {0x64, 0x10}, {0x65, 0x10}, {0x75, 0x38}};
        for (const auto &v : reset) r[v[0]] = v[1];
        for (uint8_t a = 0x1f; a <= 0x2a; a += 2) r[a] = 0x80;
    }
    uint8_t read(uint8_t reg) override
    {
        const uint8_t value = r[reg & 0x7f];
        if (reg == 0x2d) r[reg] = 0;  // INT_STATUS: cleared by the read
        return value;
    }
    void write(uint8_t reg, uint8_t value) override
    {
        const bool writable = reg == 0x11 || reg == 0x13 || reg == 0x14 || reg == 0x16 || (reg >= 0x4c && reg <= 0x50) ||
                              (reg >= 0x63 && reg <= 0x68) || reg == 0x76;
        if (!writable) return fail("write of " + hex(value) + " to read-only/unlisted " + hex(reg));
        if (reg == 0x11 && (value & 1)) return resetValues();
        r[reg] = value;
    }
    bool line() const override
    {
        return (r[0x68] & 0x08) && (r[0x2d] & 0x08) && (r[0x14] & 0x38) == 0x38 && !(r[0x64] & 0x10);
    }
    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        if ((r[0x4e] & 0x03) == 0x03) storeBigEndian(r + 0x1f, accel);
        if ((r[0x4e] & 0x0c) == 0x0c) storeBigEndian(r + 0x25, gyro);
        r[0x2d] |= 0x08;
    }
};

struct Lsm6ds3h : Chip {
    uint8_t r[0x80] = {};
    Lsm6ds3h() { resetValues(); }
    void resetValues()
    {
        memset(r, 0, sizeof(r));
        r[0x0f] = 0x69;
        r[0x12] = 0x04;  // CTRL3_C: IF_INC
    }
    uint8_t read(uint8_t reg) override
    {
        const uint8_t value = r[reg & 0x7f];
        if (reg == 0x27) r[0x1e] &= ~2;  // OUTZ_H_G read: GDA clears
        if (reg == 0x2d) r[0x1e] &= ~1;  // OUTZ_H_XL read: XLDA clears
        return value;
    }
    void write(uint8_t reg, uint8_t value) override
    {
        if (reg < 0x01 || reg > 0x19 || reg == 0x0f) return fail("write to read-only/unlisted " + hex(reg));
        if (reg == 0x12 && (value & 1)) return resetValues();
        r[reg] = value;
    }
    bool line() const override { return r[0x0e] & r[0x1e] & 0x03; }
    void produce(const int16_t accel[3], const int16_t gyro[3]) override
    {
        for (unsigned i = 0; i < 3; ++i) {
            if (r[0x11] >> 4) r[0x22 + 2 * i] = (uint8_t)gyro[i], r[0x23 + 2 * i] = (uint8_t)((uint16_t)gyro[i] >> 8);
            if (r[0x10] >> 4) r[0x28 + 2 * i] = (uint8_t)accel[i], r[0x29 + 2 * i] = (uint8_t)((uint16_t)accel[i] >> 8);
        }
        r[0x1e] |= (r[0x10] >> 4 ? 1 : 0) | (r[0x11] >> 4 ? 2 : 0);
    }
};

struct UnknownPart : Chip {
    uint8_t read(uint8_t reg) override { return reg == 0x75 ? 0x12 : reg == 0x0f ? 0x34 : 0; }
    void write(uint8_t reg, uint8_t) override { fail("write to an unidentified part: " + hex(reg)); }
    bool line() const override { return false; }
    void produce(const int16_t *, const int16_t *) override {}
};

// --- Kernel interfaces the driver calls -------------------------------------------------------------------------------
static unsigned gSpiSuspends = 0, gSpiResumes = 0;
extern "C" int tegra_spi_init(unsigned bus) { return bus == 4 ? 0 : -1; }
extern "C" void tegra_spi_suspend(unsigned bus) { gSpiSuspends += bus == 4; }
extern "C" void tegra_spi_resume(unsigned bus) { gSpiResumes += bus == 4; }
extern "C" int tegra_spi_transfer(unsigned bus, unsigned cs, const void *txv, void *rxv, size_t len)
{
    assert(bus == 4 && cs == 0 && txv && len >= 2 && len <= TEGRA_SPI_MAX_TRANSFER);
    const auto *tx = static_cast<const uint8_t *>(txv);
    auto *rx = static_cast<uint8_t *>(rxv);
    const uint8_t reg = tx[0] & 0x7f;
    gChip->access();
    if (tx[0] & 0x80) {
        assert(rx);
        gChip->ops.push_back("R " + hex(reg) + (len > 2 ? " x" + std::to_string(len - 1) : ""));
        rx[0] = 0xff;
        for (size_t i = 1; i < len; ++i) rx[i] = gChip->read((uint8_t)(reg + i - 1));
    } else {
        if (len != 2) gChip->fail("multi-byte write at " + hex(reg));
        gChip->ops.push_back("W " + hex(reg) + " " + hex(tx[1]));
        gChip->write(reg, tx[1]);
    }
    return 0;
}

static IOService gNub;  // PX2's interrupt nub
extern "C" IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned trigger)
{
    assert(gpio == TEGRA_GPIO_IMU_INT && trigger == TEGRA_GPIO_LEVEL_HIGH);
    return &gNub;
}
extern "C" void tegra_gpio_describe(unsigned gpio, char *buffer, size_t size)
{
    std::snprintf(buffer, size, "GPIO %u (stub)", gpio);
}
extern "C" void tegra_gpio_check_delivery(unsigned gpio, const char *who)
{
    if (gChip->line()) panic("%s: GPIO %u interrupt lost (stub)", who, gpio);
}

struct HidDevice : IOService {
    SwitchHIDKind kind = kSwitchHIDTouch;
    SwitchHIDFeatureHandler handler = nullptr;
    void *context = nullptr;
    std::vector<SwitchMotionReport> reports;
};
static std::vector<HidDevice *> gHid;
IOService *switch_hid_create(SwitchHIDKind kind)
{
    auto *device = new HidDevice;
    device->kind = kind;
    gHid.push_back(device);
    return device;
}
IOReturn switch_hid_post(IOService *device, const void *report, size_t length)
{
    assert(length == sizeof(SwitchMotionReport));
    SwitchMotionReport copy;
    memcpy(&copy, report, sizeof(copy));
    static_cast<HidDevice *>(device)->reports.push_back(copy);
    return kIOReturnSuccess;
}
void switch_hid_set_feature_handler(IOService *device, SwitchHIDFeatureHandler handler, void *context)
{
    static_cast<HidDevice *>(device)->handler = handler;
    static_cast<HidDevice *>(device)->context = context;
}

// --- Scenario helpers ---------------------------------------------------------------------------------------------------
static HidDevice *hid(SwitchHIDKind kind)
{
    for (HidDevice *device : gHid)
        if (device->kind == kind) return device;
    return nullptr;
}
// A client's ReportInterval (microseconds) as IOHIDEventDriver sends it: report ID 1, then the 32-bit field.
static void setInterval(SwitchHIDKind kind, uint32_t us)
{
    HidDevice *device = hid(kind);
    assert(device && device->handler);
    const uint8_t report[5] = {1, (uint8_t)us, (uint8_t)(us >> 8), (uint8_t)(us >> 16), (uint8_t)(us >> 24)};
    device->handler(device->context, report, sizeof(report));
}
static IOInterruptEventSource *dataReady()
{
    for (IOInterruptEventSource *source : IOInterruptEventSource::all)
        if (source->provider == &gNub) return source;
    return nullptr;
}
// PX2 is level-triggered: while INT2 is high and the source enabled, its action runs again (at most `limit` times).
static unsigned deliver(unsigned limit = 4)
{
    unsigned fired = 0;
    for (IOInterruptEventSource *source = dataReady(); source && source->enabled && gChip->line() && fired < limit;) {
        source->interruptOccurred(nullptr, &gNub, 0);
        ++fired;
    }
    return fired;
}
static void expectOps(const std::vector<std::string> &want, const std::string &what)
{
    const bool same = gChip->ops == want;
    check(same, what + ": register traffic");
    if (!same) {
        std::fprintf(stderr, "  want:");
        for (const std::string &op : want) std::fprintf(stderr, " [%s]", op.c_str());
        std::fprintf(stderr, "\n  got: ");
        for (const std::string &op : gChip->ops) std::fprintf(stderr, " [%s]", op.c_str());
        std::fprintf(stderr, "\n");
    }
    gChip->ops.clear();
}
static bool logged(const std::string &line)
{
    for (const std::string &l : gLog)
        if (l == line) return true;
    return false;
}
static void expectLog(const std::string &line, const std::string &what)
{
    check(logged(line), what + ": log line \"" + line + "\"");
    if (!logged(line))
        for (const std::string &l : gLog) std::fprintf(stderr, "  log: %s\n", l.c_str());
}
static bool sameReport(const SwitchMotionReport &r, int32_t x, int32_t y, int32_t z)
{
    return r.id == 1 && r.x == x && r.y == y && r.z == z;
}
static void expectReports(SwitchHIDKind kind, size_t count, int32_t x, int32_t y, int32_t z, const std::string &what)
{
    const std::vector<SwitchMotionReport> &reports = hid(kind)->reports;
    bool ok = reports.size() == count;
    for (const SwitchMotionReport &r : reports) ok = ok && sameReport(r, x, y, z);
    check(ok, what + ": " + std::to_string(count) + " reports of (" + std::to_string(x) + ", " + std::to_string(y) +
                  ", " + std::to_string(z) + ")");
    if (!ok)
        for (const SwitchMotionReport &r : reports) std::fprintf(stderr, "  report: %d %d %d\n", r.x, r.y, r.z);
}
static void expectClean(const std::string &what)
{
    check(gChip->errors.empty(), what + ": datasheet rules kept");
    for (const std::string &e : gChip->errors) std::fprintf(stderr, "  %s: %s\n", what.c_str(), e.c_str());
}
static bool start(Chip &chip)
{
    gChip = &chip;
    gLog.clear();
    return switch_motion_start() == 0;
}
static void teardown()
{
    if (gMotion) gMotion->detach(IOService::getResourceService());  // the attachment's reference: frees it
    gMotion = nullptr;
    IOInterruptEventSource::all.clear();
    IOTimerEventSource::last = nullptr;
    for (HidDevice *device : gHid) device->release();
    gHid.clear();
    gLog.clear();
    gChip = nullptr;
}

// What start() sends an ICM-42607 after identifying it: reset (TDK inv_imu_device_reset), interface and INT2 set-up,
// then both sensors off at the lowest rate (12.5 Hz: ODR 12, 16 Hz filter).
static const std::vector<std::string> k42607Start = {
    "R 75",    "R 0f",                                       // identify, nothing written first
    "W 7c 00", "W 79 00", "W 02 10", "W 01 04", "R 3a",      // reset, 4-wire restated, RESET_DONE
    "R 36",    "W 36 41", "W 05 05", "R 35",    "W 35 33",   // I3C off + PLL, SPI slew, I2C off + big-endian
    "W 06 38", "W 2b 00",                                    // INT2 latched push-pull high; INT1 nothing
    "W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00",  // off
};

// --- Scenarios ------------------------------------------------------------------------------------------------------
static void decodeAndMapping()
{
    // ICM: big-endian accelerometer then gyroscope; -32768 marks a sensor that is off.
    const uint8_t icm[12] = {0x00, 0x64, 0xff, 0x38, 0x10, 0x00, 0x00, 0xa4, 0xfe, 0xb8, 0x00, 0x10};
    int16_t accel[3], gyro[3];
    bool fresh[2];
    decodeICM(icm, true, accel, gyro, fresh);
    check(accel[0] == 100 && accel[1] == -200 && accel[2] == 4096 && gyro[0] == 164 && gyro[1] == -328 &&
              gyro[2] == 16 && fresh[Accel] && fresh[Gyro],
          "decodeICM: big-endian words");
    decodeICM(icm, false, accel, gyro, fresh);
    check(!fresh[Accel] && !fresh[Gyro], "decodeICM: no data-ready, nothing fresh");
    uint8_t off[12];
    memcpy(off, icm, sizeof(off));
    off[6] = 0x80, off[7] = 0x00;
    decodeICM(off, true, accel, gyro, fresh);
    check(fresh[Accel] && !fresh[Gyro], "decodeICM: a gyroscope axis at -32768 is not fresh");

    // ST: STATUS, then gyroscope and accelerometer little-endian from byte 4.
    const uint8_t stb[16] = {0x01, 0, 0, 0, 0xa4, 0x00, 0xb8, 0xfe, 0x10, 0x00, 0x64, 0x00, 0x38, 0xff, 0xfe, 0xef};
    decodeST(stb, accel, gyro, fresh);
    check(accel[0] == 100 && accel[1] == -200 && accel[2] == -4098 && gyro[0] == 164 && gyro[1] == -328 &&
              gyro[2] == 16 && fresh[Accel] && !fresh[Gyro],
          "decodeST: little-endian words, STATUS bits");

    // Device frame (portrait panel: +X = console bottom, +Y = console right, +Z out of the screen), gravity vector.
    // ICM (X = -y, Y = +x, Z = +z): chip +z out of the screen, so lying face up (+1 g on z) reads z = -1 g.
    const int16_t up[3] = {0, 0, 4096}, chipX[3] = {4096, 0, 0}, chipY[3] = {0, 4096, 0}, a[3] = {100, -200, 4096};
    const auto r = [](const FamilyInfo &f, Sensor s, const int16_t *v) { return deviceReport(f, s, v); };
    check(sameReport(r(kICM42607, Accel, up), 0, 0, -1000000), "ICM-42607 face up: z = -1 g");
    check(sameReport(r(kICM42607, Accel, chipX), 1000000, 0, 0), "ICM-42607 chip +x up: device x = +1 g (bottom)");
    check(sameReport(r(kICM42607, Accel, chipY), 0, 1000000, 0), "ICM-42607 chip +y up: device y = +1 g (right)");
    check(sameReport(r(kICM42607, Accel, a), 24414, -48828, -1000000), "ICM-42607 accel scale 1e6/4096 ug");
    const int16_t g[3] = {164, -328, 16};
    check(sameReport(r(kICM42607, Gyro, g), -174532, 349065, 17027), "ICM-42607 gyro: rad/s, rotated not negated");
    check(sameReport(r(kICM40607, Accel, a), 24414, -48828, -1000000), "ICM-40607 shares the ICM mount");
    // ST (X = +y, Y = +x, Z = -z): chip -z out of the screen.
    const int16_t s[3] = {100, -200, -4098};
    check(sameReport(r(kST, Accel, s), 24400, 48800, -999912), "ST accel: mount and 0.244 mg/LSB");

    // Rates: the ICM-42607's ODR codes are one above the ICM-40607's.
    const uint8_t codes42607[5] = {12, 11, 10, 9, 8}, codes40607[5] = {11, 10, 9, 8, 7};
    bool rates = true;
    for (unsigned i = 0; i < 5; ++i) {
        rates = rates && kICM42607.rates[i].code == codes42607[i] && kICM40607.rates[i].code == codes40607[i] &&
                kICM42607.rates[i].milliHz == kICM40607.rates[i].milliHz;
    }
    check(rates, "ODR codes: ICM-42607 12.5 Hz = 12 .. 200 Hz = 8; ICM-40607 11 .. 7");
}

// The r7p8 console: an ICM-42607-P straight from power-on (0x0f = 0x80, 0x75 = 0x60), through a whole client session.
static void icm42607Session()
{
    Icm42607 chip;
    check(start(chip), "42607: started");
    check(IOService::getResourceService() && gMotion && gMotion->imu == "ICM-42607-P", "42607: IMU property");
    expectOps(k42607Start, "42607 start");
    expectLog("IOLog: SwitchMotion: started (ICM-42607-P on SPI4, data-ready on PX2)", "42607 start");
    check(dataReady() && !dataReady()->enabled, "42607: data-ready masked while idle");

    // UIAccelerometer-style client at 100 Hz: ODR code 9 (not the ICM-40607's 8), 25 Hz filter, accelerometer LN.
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 20 09", "W 21 29", "W 23 36", "W 24 46", "W 1f 03", "W 2d 08"}, "42607 accel 100 Hz");
    expectLog("os_log: SwitchMotion: 100000 mHz, accel 1 (10000 us), gyro 0 (0 us)", "42607 accel 100 Hz");
    check(dataReady()->enabled && IOTimerEventSource::last->armed && IOTimerEventSource::last->ms == 1000,
          "42607: data-ready and the 1 s watchdog armed");

    const int16_t accel[3] = {100, -200, 4096}, gyro[3] = {164, -328, 16}, none[3] = {0, 0, 0};
    gNow += 10000000;  // 10 ms: inside the accelerometer's 20 ms settle
    chip.produce(accel, none);
    check(deliver() == 1, "42607: one interrupt per sample (the status read drops INT2)");
    expectOps({"R 39", "R 0b x12"}, "42607 sample: status, then data");
    check(hid(kSwitchHIDAccelerometer)->reports.empty(), "42607: samples inside the settle time dropped");
    gNow += 15000000;
    chip.produce(accel, none);
    check(deliver() == 1, "42607: second sample");
    chip.produce(accel, none);
    check(deliver() == 1, "42607: third sample");
    gChip->ops.clear();
    expectReports(kSwitchHIDAccelerometer, 2, 24414, -48828, -1000000, "42607 accel reports");
    expectLog("IOLog: SwitchMotion: ICM-42607-P first accelerometer sample: raw 100 -200 4096, device frame 24 -48 -1000 "
              "mg (face up: z near -1000; upright: x near +1000)",
              "42607 first sample");
    unsigned firstLines = 0;
    for (const std::string &l : gLog) firstLines += l.find("first accelerometer sample") != std::string::npos;
    check(firstLines == 1, "42607: the first sample is logged once");
    IOTimerEventSource::last->fire();
    check(IOTimerEventSource::last->armed, "42607: watchdog re-armed after samples");

    // CoreMotion's gyroscope at 200 Hz joins: both at ODR 8 with the 53 Hz filter; the accelerometer is decimated 2:1.
    setInterval(kSwitchHIDGyro, 5000);
    expectOps({"W 20 08", "W 21 28", "W 23 34", "W 24 44", "W 1f 0f", "W 2d 08"}, "42607 gyro 200 Hz");
    expectLog("os_log: SwitchMotion: 200000 mHz, accel 1 (10000 us), gyro 1 (5000 us)", "42607 gyro 200 Hz");
    gNow += 30000000;  // inside the gyroscope's 60 ms
    chip.produce(accel, gyro);
    deliver();
    check(hid(kSwitchHIDGyro)->reports.empty(), "42607: gyroscope samples inside its 60 ms settle dropped");
    gNow += 31000000;
    hid(kSwitchHIDAccelerometer)->reports.clear();
    for (unsigned i = 0; i < 4; ++i) {
        chip.produce(accel, gyro);
        deliver();
    }
    gChip->ops.clear();
    expectReports(kSwitchHIDGyro, 4, -174532, 349065, 17027, "42607 gyro reports");
    expectReports(kSwitchHIDAccelerometer, 2, 24414, -48828, -1000000, "42607 accel decimated to 100 Hz");

    // Both clients gone: routing off first, then the sensors (at the lowest rate's settings, as at start);
    // data-ready masked, watchdog cancelled.
    setInterval(kSwitchHIDGyro, 0);
    expectOps({"W 20 09", "W 21 29", "W 23 36", "W 24 46", "W 1f 03", "W 2d 08"}, "42607 gyro gone");
    setInterval(kSwitchHIDAccelerometer, 0);
    expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "42607 off");
    expectLog("os_log: SwitchMotion: off", "42607 off");
    check(!dataReady()->enabled && !IOTimerEventSource::last->armed, "42607: idle again");

    // System sleep and wake: the part off, SPI4 clock gated; on wake the pin set-up again and the client's rate back.
    setInterval(kSwitchHIDAccelerometer, 40000);
    expectOps({"W 20 0b", "W 21 2b", "W 23 37", "W 24 47", "W 1f 03", "W 2d 08"}, "42607 accel 25 Hz");
    switch_motion_suspend();
    expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "42607 suspend");
    switch_motion_resume();
    expectOps({"R 36", "W 36 41", "W 05 05", "R 35", "W 35 33", "W 06 38", "W 2b 00", "W 2d 00", "W 20 0c", "W 21 2c",
               "W 23 37", "W 24 47", "W 1f 00", "W 20 0b", "W 21 2b", "W 23 37", "W 24 47", "W 1f 03", "W 2d 08"},
              "42607 resume");
    check(gSpiSuspends == 1 && gSpiResumes == 1 && dataReady()->enabled, "42607: SPI4 suspended and resumed");
    expectClean("42607 session");
    teardown();
}

// Left running by an earlier OS: both sensors on, a latched data-ready, an MREG block selected, and ACCEL_DATA_Z1 (0x0f)
// holding 0x69, LSM6DS3H's WHO_AM_I. Still an ICM-42607-P, and the same reset and set-up.
static void icm42607LeftRunning()
{
    Icm42607 chip;
    const int16_t accel[3] = {0, 0, 0x6900}, gyro[3] = {1, 2, 3};
    chip.r[0x1f] = 0x0f, chip.r[0x20] = 0x09, chip.r[0x21] = 0x09, chip.r[0x06] = 0x38, chip.r[0x2d] = 0x08;
    chip.r[0x3a] = 0, chip.r[0x79] = 0x50, chip.r[0x7c] = 0x50;
    chip.produce(accel, gyro);
    check(chip.r[0x0f] == 0x69 && chip.line(), "left running: 0x0f reads 0x69, INT2 high");
    check(start(chip), "left running: started");
    check(gMotion && gMotion->imu == "ICM-42607-P", "left running: identified by 0x75, not 0x0f");
    expectOps(k42607Start, "left running start");
    check(chip.r[0x1f] == 0 && chip.r[0x79] == 0 && chip.r[0x7c] == 0 && !chip.line(), "left running: reset");
    expectClean("left running");
    teardown();
}

static void icm42607Faults()
{
    {  // A part that never raises data-ready: one line, motion off for the boot, no panic.
        Icm42607 chip;
        check(start(chip), "no data-ready: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        gChip->ops.clear();
        IOTimerEventSource::last->fire();
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P raised no data-ready on PX2 in 1000 ms at 100.0 Hz (accel 1, "
                  "gyro 0): INT2 not wired or the part stopped",
                  "no data-ready");
        expectOps({"W 2d 00", "W 20 0c", "W 21 2c", "W 23 37", "W 24 47", "W 1f 00"}, "no data-ready: part off");
        check(!dataReady()->enabled && !IOTimerEventSource::last->armed, "no data-ready: masked, no watchdog");
        setInterval(kSwitchHIDGyro, 5000);
        expectOps({}, "no data-ready: off for the boot");
        expectClean("no data-ready");
        teardown();
    }
    {  // INT2 high but the interrupt never arrived: the lost-interrupt panic.
        Icm42607 chip;
        check(start(chip), "lost interrupt: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        const int16_t accel[3] = {1, 2, 3}, none[3] = {};
        chip.produce(accel, none);
        std::string message;
        try {
            IOTimerEventSource::last->fire();
        } catch (const Panic &p) {
            message = p.what();
        }
        check(message == "SwitchMotion: GPIO 186 interrupt lost (stub)", "lost interrupt: panics (" + message + ")");
        teardown();
    }
    {  // INT2 stuck high with no data: 64 empty interrupts allowed, the 65th turns motion off.
        Icm42607 chip;
        check(start(chip), "stuck: started");
        setInterval(kSwitchHIDAccelerometer, 10000);
        chip.stuck = true;
        check(deliver(100) == 65, "stuck: off after 65 empty interrupts");
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P data-ready (PX2) fired 65 times in a row with no new data: "
                  "GPIO 186 (stub)",
                  "stuck");
        check(!dataReady()->enabled, "stuck: masked");
        teardown();
    }
    {  // A reset that never completes: one line with the registers, nothing started.
        Icm42607 chip;
        chip.resetBroken = true;
        check(!start(chip), "reset fails: not started");
        expectOps({"R 75", "R 0f", "W 7c 00", "W 79 00", "W 02 10", "W 01 04", "R 3a", "R 01", "R 75"},
                  "reset fails");
        expectLog("IOLog: SwitchMotion: off: ICM-42607-P software reset did not complete (INT_STATUS 0, DEVICE_CONFIG "
                  "0x4, WHO_AM_I now 0x60)",
                  "reset fails");
        check(!gMotion && gHid.empty(), "reset fails: nothing published");
        teardown();
    }
    {  // ICM-42607 (WHO_AM_I 0x67, also the ICM-42670-P): the same map.
        Icm42607 chip(0x67);
        check(start(chip), "0x67: started");
        check(gMotion && gMotion->imu == "ICM-42607", "0x67: ICM-42607");
        expectOps(k42607Start, "0x67 start");
        teardown();
    }
    {  // Nothing known: both WHO_AM_I values in one line, not a single write.
        UnknownPart chip;
        check(!start(chip), "unknown: not started");
        expectOps({"R 75", "R 0f"}, "unknown: reads only");
        expectLog("IOLog: SwitchMotion: off: no IMU on SPI4 CS0: WHO_AM_I 0x75 = 0x12, 0x0f = 0x34 (expected "
                  "ICM-42607-P 0x60, ICM-42607 0x67, ICM-40607 0x38 at 0x75; LSM6DS3H 0x69, LSM6DSE 0x6a, LSM6DSO 0x6c "
                  "at 0x0f); SCK pad GPIO 21 (stub)",
                  "unknown");
        expectClean("unknown");
        teardown();
    }
}

static void icm40607Session()
{
    Icm40607 chip;
    check(start(chip), "40607: started");
    check(gMotion && gMotion->imu == "ICM-40607", "40607: identified");
    expectOps({"R 75", "R 0f", "W 11 01", "R 2d", "W 16 00", "R 13", "W 13 05", "R 4c", "W 4c 33", "W 14 38", "R 64",
               "W 64 00", "W 65 00", "W 68 00", "W 4f 0b", "W 50 2b", "W 4e 20"},
              "40607 start (no bank-1 INTF_CONFIG4 write)");
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 4f 08", "W 50 28", "W 4e 23", "W 68 08"}, "40607 accel 100 Hz");
    gNow += 25000000;
    const int16_t accel[3] = {100, -200, 4096}, none[3] = {};
    chip.produce(accel, none);
    check(deliver() == 1, "40607: one interrupt per sample");
    expectOps({"R 1f x15"}, "40607 sample burst");
    expectReports(kSwitchHIDAccelerometer, 1, 24414, -48828, -1000000, "40607 accel report");
    expectClean("40607");
    teardown();
}

static void lsm6ds3hSession()
{
    Lsm6ds3h chip;
    check(start(chip), "LSM6DS3H: started");
    check(gMotion && gMotion->imu == "LSM6DS3H", "LSM6DS3H: identified");
    expectOps({"R 75", "R 0f", "W 12 01", "R 12", "W 0a 00", "W 12 44", "W 13 08", "W 0d 00", "W 0e 00", "W 10 00",
               "W 11 00"},
              "LSM6DS3H start");
    setInterval(kSwitchHIDAccelerometer, 10000);
    expectOps({"W 10 4c", "W 11 00", "W 0e 01"}, "LSM6DS3H accel 104 Hz");
    gNow += 25000000;
    const int16_t accel[3] = {100, -200, -4098}, none[3] = {};
    chip.produce(accel, none);
    check(deliver() == 1, "LSM6DS3H: one interrupt per sample");
    expectOps({"R 1e x16"}, "LSM6DS3H sample burst");
    expectReports(kSwitchHIDAccelerometer, 1, 24400, 48800, -999912, "LSM6DS3H accel report");
    expectClean("LSM6DS3H");
    teardown();
}

int main()
{
    decodeAndMapping();
    icm42607Session();
    icm42607LeftRunning();
    icm42607Faults();
    icm40607Session();
    lsm6ds3hSession();
    if (gFailures) {
        std::printf("FAIL: switch motion: %u of %u checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("PASS: switch motion (%u checks)\n", gChecks);
    return 0;
}
