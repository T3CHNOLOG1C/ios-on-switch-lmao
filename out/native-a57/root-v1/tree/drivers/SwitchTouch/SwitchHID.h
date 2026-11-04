// SPDX-License-Identifier: BSD-2-Clause-Patent
// HID devices for the Switch's touch screen, buttons, ambient light sensor and motion sensors (SwitchHIDDevice.cpp), fed
// by SwitchTouch.cpp, ../SwitchButtons/SwitchButtons.cpp, ../SwitchBrightness/SwitchALS.cpp and
// ../SwitchMotion/SwitchMotion.cpp.
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
#include "../NativeMobileDisplay/IOSwitchPortrait.h"

class IOService;
class OSObject;

enum SwitchHIDKind : unsigned {
    kSwitchHIDTouch = 0,
    kSwitchHIDButtons = 1,
    kSwitchHIDLight = 2,
    kSwitchHIDAccelerometer = 3,
    kSwitchHIDGyro = 4,
    kSwitchHIDOrientation = 5,
};

// Touch screen, report ID 1: ten finger slots, slot n always carries contact n. Coordinates are panel pixels in
// the landscape frame iOS draws in (0..1279 left to right, 0..719 top to bottom); the descriptor's logical range
// is the whole panel, so backboardd scales them to whatever logical size the UI uses.
constexpr unsigned kSwitchTouchSlots = 10, kSwitchTouchWidth = 1280, kSwitchTouchHeight = 720;
// Portrait-native (IOSwitchPortrait.h): reports carry the panel's own frame instead, the one iOS is given as its screen
// (0..719 left to right, 0..1279 top to bottom of the portrait panel): landscape (x, y) is reported as (y, 1279 - x),
// the display's own mapping. backboardd's digitizer-to-panel rotation is 0, and iOS turns touches into its landscape
// interface itself.
constexpr unsigned kSwitchTouchReportWidth = IOSWITCH_PORTRAIT_NATIVE ? kSwitchTouchHeight : kSwitchTouchWidth,
                   kSwitchTouchReportHeight = IOSWITCH_PORTRAIT_NATIVE ? kSwitchTouchWidth : kSwitchTouchHeight;
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

// Motion sensors (../SwitchMotion/SwitchMotion.cpp). Each is a top-level collection with the usage pair iPadOS 17's
// CoreMotion (CLIoHidInterface) and backboardd match, as the J120 device tree's AOP accel and gyro nodes declare
// (device-usage-page 0xff00, device-usage 3 and 9):
//  * Accelerometer, Apple vendor 0xff00/0x0003: input report 1 = Sensor-page Acceleration X/Y/Z (0x0453-0x0455) in
//    micro-g (unit exponent -6, so IOHIDEventDriver's kIOHIDEventTypeAccelerometer events carry g), feature report 1 =
//    Sensor Report Interval (0x030e, microseconds).
//  * Gyroscope, Apple vendor 0xff00/0x0009: the same with Angular Velocity X/Y/Z (0x0457-0x0459) in micro-radians per
//    second (kIOHIDEventTypeGyro in rad/s).
//  * Device orientation, Sensor 0x20/0x008a (what CoreMotion's orientation notifier in backboardd matches): input
//    report 1 = one bit per AppleVendorMotion (0xff0c) usage 0x64 + UIDeviceOrientation (0x64 unknown .. 0x6a face
//    down); IOHIDEventDriver turns a bit that goes 0 -> 1 into a kIOHIDEventTypeOrientation event (CMUsage).
// Vectors are in iPadOS's device frame: the portrait panel's (IOSwitchPortrait.h), +X to its right, +Y to its top, +Z
// out of the screen; acceleration is the gravity vector (lying face up reads z = -1 g), as Apple's AOP reports it.
struct __attribute__((packed)) SwitchMotionReport {
    uint8_t id;  // 1
    int32_t x, y, z;
};
struct __attribute__((packed)) SwitchOrientationReport {
    uint8_t id;           // 1
    uint8_t orientation;  // 1u << UIDeviceOrientation
};

// Creates, attaches (under IOResources) and starts the device. The first call loads IOHIDFamily, registers the
// device class and adds the IOHIDEventDriver personalities. Thread context; panics with the reason on failure.
IOService *switch_hid_create(SwitchHIDKind kind);

// Hands one input report to the device (and from there synchronously to its IOHIDEventDriver). Reports sent before
// the event driver has opened the device are dropped by IOHIDFamily. Call from one thread per device.
IOReturn switch_hid_post(IOService *device, const void *report, size_t length);

// Hands an IOHIDEvent to the device's IOHIDEventDriver as if the driver had made it (IOHIDEventService::dispatchEvent,
// the driver's slot 198). kIOReturnNotReady until the driver has opened the device. Call from one thread per device.
IOReturn switch_hid_dispatch_event(IOService *device, OSObject *event);

// Feature reports the event system sends the device: IOHIDEventDriver::setProperties writes a client's ReportInterval
// (microseconds; 0 when the last client stops) into the Report Interval element, and IOHIDDevice::postElementValues
// hands that feature report to the device's setReport (slot 183). Called with the report's bytes (the report ID first
// when the report carries one) on the thread that set the property, which may hold IOHIDEventDriver's work loop gate:
// it must not wait for anything. Devices without a handler answer kIOReturnUnsupported, as the original setReport does.
typedef void (*SwitchHIDFeatureHandler)(void *context, const uint8_t *report, size_t length);
void switch_hid_set_feature_handler(IOService *device, SwitchHIDFeatureHandler handler, void *context);

// Touch only: bit n set = contact n began within the edge band. Its finger events then carry
// kIOHIDDigitizerEventFromEdgeTip, which UIKit needs for the home and app-switcher edge gestures.
void switch_hid_set_edge_contacts(IOService *device, uint32_t contacts);
