// SPDX-License-Identifier: BSD-2-Clause-Patent
// The device half of system sleep (SwitchSleepDevices.cpp): every driver's sleep and wake hook in the order
// SwitchSuspend.h lists, each a power-trail step (SwitchSleepSteps.h) and a power-journal line with its time. Thread
// context, TegraI2C up. SwitchSuspend.cpp runs the SoC's part around them.
#pragma once
#include <stdint.h>
#include <kern/clock.h>

// From the ambient light sensor to the gauge's alert; true when the Bluetooth controller was set up to wake the
// console for this sleep.
bool switch_sleep_devices(void);
// The journal to Data, then the SD down: the last durable record of the sleep, after the wake events and the BPMP.
void switch_sleep_storage(void);
// One step: its mark, the hook, its time in the journal.
void switch_sleep_timed(uint8_t step, void (*hook)(void));
// From the security engines to the ambient light sensor.
void switch_wake_devices(void);

static inline uint64_t switch_sleep_ns_since(uint64_t start)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return ns;
}
