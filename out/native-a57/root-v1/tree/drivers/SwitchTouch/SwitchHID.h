// SPDX-License-Identifier: BSD-2-Clause-Patent
// HID devices for the Switch's touch screen and buttons (SwitchHIDDevice.cpp), fed by SwitchTouch.cpp and
// ../SwitchButtons/SwitchButtons.cpp.
//
// Each device is an IOHIDDevice (the original class, through NativeHidInput's bindings) with a fixed report
// descriptor. Apple's in-kernel IOHIDEventDriver turns its reports into IOHIDEvents for backboardd, exactly as for a
// real HID transport. The DriverKit AppleUserHIDEventDriver (AppleUserHIDDrivers.dext) also matches IOHIDInterface
// for these usage pages, at IOProbeScore 1; it never starts here because this module adds its own IOHIDEventDriver
// personality per device (VendorID/ProductID match, IOProbeScore 10000, no IOMatchDefer), and IOService's
// probeCandidates skips a dext once the match category is consumed (canTerminateForReplacement needs
// IOPrimaryDriverTerminateOptions, which IOHIDEventDriver lacks).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOReturn.h>

class IOService;
class OSObject;

enum SwitchHIDKind : unsigned { kSwitchHIDTouch = 0, kSwitchHIDButtons = 1, kSwitchHIDLight = 2 };

// Touch screen, report ID 1: ten finger slots, slot n always carries contact n. Coordinates are panel pixels in
// the landscape frame iOS draws in (0..1279 left to right, 0..719 top to bottom); the descriptor's logical range
// is the whole panel, so backboardd scales them to whatever logical size the UI uses.
constexpr unsigned kSwitchTouchSlots = 10, kSwitchTouchWidth = 1280, kSwitchTouchHeight = 720;
enum : uint8_t { kSwitchTouchTip = 1u << 0, kSwitchTouchValid = 1u << 1 };  // bit 2 (Untouch) stays 0
struct __attribute__((packed)) SwitchTouchSlot {
    uint8_t flags, contact;
    uint16_t x, y;
};
struct __attribute__((packed)) SwitchTouchReport {
    uint8_t id;  // 1
    SwitchTouchSlot slot[kSwitchTouchSlots];
};

// Buttons, report ID 1: one bit per Consumer usage (Power 0x30, Volume Increment 0xE9, Volume Decrement 0xEA).
enum : uint8_t { kSwitchButtonPower = 1u << 0, kSwitchButtonVolumeUp = 1u << 1, kSwitchButtonVolumeDown = 1u << 2 };
struct __attribute__((packed)) SwitchButtonsReport {
    uint8_t id;  // 1
    uint8_t buttons;
};

// Ambient light sensor (../SwitchBrightness/SwitchALS.cpp): top-level collection Apple vendor 0xff00 / AmbientLightSensor
// 0x0004, CoreBrightness's ALS usage pair, with one Sensor-page Report Interval feature (IOHIDEventDriver starts only when
// it recognizes an element, and parseSensorPropertyElement takes that one). It posts no input reports: IOHIDEventDriver
// has no ambient light element, so SwitchALS makes the kIOHIDEventTypeAmbientLightSensor events itself and hands them to
// switch_hid_dispatch_event.

// Creates, attaches (under IOResources) and starts the device. The first call loads IOHIDFamily, registers the
// device class and adds the IOHIDEventDriver personalities. Thread context; panics with the reason on failure.
IOService *switch_hid_create(SwitchHIDKind kind);

// Hands one input report to the device (and from there synchronously to its IOHIDEventDriver). Reports sent before
// the event driver has opened the device are dropped by IOHIDFamily. Call from one thread per device.
IOReturn switch_hid_post(IOService *device, const void *report, size_t length);

// Hands an IOHIDEvent to the device's IOHIDEventDriver as if the driver had made it (IOHIDEventService::dispatchEvent,
// the driver's slot 198). kIOReturnNotReady until the driver has opened the device. Call from one thread per device.
IOReturn switch_hid_dispatch_event(IOService *device, OSObject *event);

// Touch only: bit n set = contact n began within the edge band. Its finger events then carry
// kIOHIDDigitizerEventFromEdgeTip, which UIKit needs for the home and app-switcher edge gestures.
void switch_hid_set_edge_contacts(IOService *device, uint32_t contacts);
