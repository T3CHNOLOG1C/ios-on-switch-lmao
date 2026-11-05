// The kernel side of thermalmonitord's sensors (SwitchSensors.h's table is published as HID temperature services by
// drivers/SwitchTouch/SwitchHIDDevice.cpp): the descriptor and IOHIDEventService/IOHIDEvent vtable slots it relies on
// are Apple's binary interface (iPadOS 21H461's IOHIDFamily 2031.120.4), which no host model can call, so the
// source is checked against the recorded values. SwitchALS uses the same IOHIDEvent slots.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using ioswitch::test::SourceText;
using testing::ContainsRegex;
using testing::HasSubstr;

TEST(SwitchSensorsHidSource, HidShapes)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    // Apple vendor 0xff00 / TemperatureSensor 0x0005: the pair thermalmonitord matches (createHIDEventSystemObject).
    EXPECT_THAT(hid, HasSubstr("0x06, 0x00, 0xff, 0x09, 0x05, 0xa1, 0x01"));
    EXPECT_THAT(hid, ContainsRegex("0x5108, 0xff00, 0x05, \"Switch Temperature Sensor\""));
    // IOHIDEventService reserved 8..11 in IOHIDEventService.h order (2031.120.4): dispatchEvent 198 (proven by the edge
    // wrapper), getPrimaryUsagePage 199, getPrimaryUsage 200, copyEvent 201.
    EXPECT_THAT(hid, HasSubstr("kDispatch = 198"));
    EXPECT_THAT(hid, HasSubstr("kPrimaryUsagePage = 199, kPrimaryUsage = 200, kCopyEvent = 201"));
    // IOHIDEvent: setTimeStamp 17, getType 18, setType 19 (as SwitchALS), getFixedValue 25, setFixedValue 26.
    EXPECT_THAT(hid, HasSubstr("kSetTimeStamp = 17, kGetType = 18, kSetType = 19, kGetFixed = 25, kSetFixed = 26"));
    EXPECT_THAT(hid, HasSubstr("kType = 15, kFieldLevel = 0xf0000"));  // what IOHIDEventGetFloatValue(e, 0xf0000) reads
    // The publish notification matches every IOHIDEventDriver: one with an IOPropertyMatch on VendorID/ProductID never
    // fired for the temperature drivers on hardware.
    EXPECT_THAT(hid, HasSubstr("addMatchingNotification(gIOFirstPublishNotification, matching, &published"));
    EXPECT_THAT(hid, testing::Not(HasSubstr("gIOPropertyMatchKey")));
    const std::string als = SourceText("../../SwitchBrightness/SwitchALS.cpp");
    EXPECT_THAT(als, HasSubstr("kSetTimeStamp = 17, kSetType = 19, kSetIntegerValue = 24"));
}
