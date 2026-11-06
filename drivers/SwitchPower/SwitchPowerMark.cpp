// SPDX-License-Identifier: BSD-2-Clause-Patent
// The durable power mark (diagnostics/switch/SwitchPowerTrail.h): one code byte in the MAX77620 RTC's ALARM2, the one
// store on the board that a forced power-off keeps, since the RTC runs from the battery while the PMIC is off.
// ALARM2_SEC holds the code, ALARM2_MIN how many marks the boot wrote (mod 128), ALARM2_YEAR kMagic. Each field's bit 7,
// its compare enable, stays clear, so the alarm never matches (Linux max77686_rtc_stop_alarm turns an alarm off the same
// way, and the power-off clears ONOFFCNFG2 WK_ALARM2 besides); ALARM1 is the wake alarm (SwitchPMIC.cpp). Horizon
// programs ALARM2 for its own alarms, so a Horizon boot in between leaves no mark of ours; hekate's
// max77620_rtc_stop_alarm clears only the compare bits and keeps the mark.
// Debug kernels write marks; every kernel keeps the RTC's update rule (SwitchMAX77620.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "SwitchMAX77620.h"

namespace {
uint64_t gQuietAt;  // mach_absolute_time() from which the RTC takes register writes and updates again (atomic)
}  // namespace

void max77620::rtcQuiet(bool canSleep)
{
    for (;;) {
        const uint64_t now = mach_absolute_time(), quiet = __atomic_load_n(&gQuietAt, __ATOMIC_ACQUIRE);
        if (now >= quiet)
            return;
        if (!canSleep)
            continue;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(quiet - now, &ns);
        IOSleep((unsigned)(ns / 1000000) + 1);
    }
}

void max77620::rtcUpdated()
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(uint64_t{kRtcUpdateMS} * 1000000, &interval);
    __atomic_store_n(&gQuietAt, mach_absolute_time() + interval, __ATOMIC_RELEASE);
}

#if DEVELOPMENT || DEBUG
namespace {
using namespace max77620;

constexpr uint8_t kMagic = 0x5a;
constexpr uint8_t kAlarmCompare = 1u << 7;
constexpr uint32_t kNoMark = 0x100;  // gWritten before this boot's first mark

uint32_t gPending = kNoMark;  // the latest mark asked for (atomic)
uint32_t gWritten = kNoMark;  // the latest mark in the RTC (gLock, or the one caller of switch_pmic_mark_now)
uint8_t gCount;               // marks written this boot
uint8_t gUpdate0;             // RTCUPDATE0's other bits as the boot found them
IOLock *gLock;
thread_call_t gCall;

using Write = int (*)(uint8_t reg, uint8_t value);

int threadWrite(uint8_t reg, uint8_t value)
{
    return writeByte(kRtc, reg, value);
}

int rawWrite(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return tegra_i2c_raw_write(kBus, kRtc, data, sizeof(data));
}

// The code and the count, then UDR. A boot's first mark writes every field, compare bits clear: whatever alarm ALARM2
// held is off from then on.
int writeMark(Write put, uint32_t code)
{
    const uint8_t count = (uint8_t)((gCount + 1u) & 0x7fu);
    int err = put(RTC_ALARM2 + SEC, (uint8_t)code);
    if (!err)
        err = put(RTC_ALARM2 + MIN, count);
    for (unsigned field = HOUR; !err && gWritten == kNoMark && field < kTimeRegs; ++field)
        err = put((uint8_t)(RTC_ALARM2 + field), field == YEAR ? kMagic : 0);
    if (!err)
        err = put(RTC_UPDATE0, gUpdate0 | kRtcWriteUpdate);
    rtcUpdated();
    if (!err) {
        gCount = count;
        gWritten = code;
    }
    return err;
}

// Writes marks until the latest is in; under gLock.
void commitLocked()
{
    for (;;) {
        const uint32_t code = __atomic_load_n(&gPending, __ATOMIC_ACQUIRE);
        if (code == gWritten || code == kNoMark)
            return;
        rtcQuiet(true);
        if (const int err = writeMark(threadWrite, code)) {
            IOLog("SwitchPowerTrail: durable mark %#04x not written (I2C5 %d)\n", code, err);
            return;
        }
    }
}

void writerCall(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    commitLocked();
    IOLockUnlock(gLock);
}

// The last boot's mark, read through RBUDR; -1 if ALARM2 holds none of ours.
int lastMark(unsigned *count)
{
    uint8_t alarm[kTimeRegs] = {};
    rtcQuiet(true);
    int err = writeByte(kRtc, RTC_UPDATE0, gUpdate0 | kRtcReadUpdate);
    if (!err) {
        rtcUpdated();
        rtcQuiet(true);
        err = tegra_i2c_read(kBus, kRtc, RTC_ALARM2, alarm, sizeof(alarm));
    }
    for (const uint8_t field : alarm) {
        if (field & kAlarmCompare)
            return -1;
    }
    if (err || alarm[YEAR] != kMagic)
        return -1;
    *count = alarm[MIN];
    return alarm[SEC];
}
}  // namespace

// SwitchPMIC.cpp's start, once the MAX77620 answers (thread context): the last boot's mark is logged and this boot's
// first replaces it.
extern "C" void switch_pmic_mark_start(void)
{
    uint8_t update = 0;
    if (const int err = readByte(kRtc, RTC_UPDATE0, &update)) {
        IOLog("SwitchPowerTrail: no durable marks this boot (RTCUPDATE0: I2C5 %d)\n", err);
        return;
    }
    gUpdate0 = update & (uint8_t)~(kRtcWriteUpdate | kRtcReadUpdate);
    unsigned count = 0;
    const int last = lastMark(&count);
    switch_power_trail_durable(last, count);
    gLock = IOLockAlloc();
    gCall =
        gLock ? thread_call_allocate_with_options(writerCall, nullptr, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0) : nullptr;
    if (!gCall) {
        IOLog("SwitchPowerTrail: no durable marks this boot (no lock or thread call)\n");
        return;
    }
    switch_power_trail_mark(SWITCH_MARK_BOOT_PMIC);
    switch_pmic_mark_flush();
}

extern "C" void switch_pmic_mark(uint8_t code)
{
    __atomic_store_n(&gPending, code, __ATOMIC_RELEASE);
    if (gCall)
        thread_call_enter(gCall);
}

extern "C" void switch_pmic_mark_flush(void)
{
    if (!gLock)
        return;
    IOLockLock(gLock);
    commitLocked();
    rtcQuiet(true);
    IOLockUnlock(gLock);
}

extern "C" void switch_pmic_mark_now(uint8_t code)
{
    __atomic_store_n(&gPending, code, __ATOMIC_RELEASE);
    if (!gCall)
        return;
    rtcQuiet(false);
    (void)writeMark(rawWrite, code);
}
#endif
