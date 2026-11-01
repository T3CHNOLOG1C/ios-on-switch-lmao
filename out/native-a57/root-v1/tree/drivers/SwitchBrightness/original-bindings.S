// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleARMBacklight::gMetaClass of the original com.apple.driver.AppleARMPlatform, at its address in OUR kernel:
// the prelink moves every original kext 16 MiB down (out/cmake/kernel/kernel.json "slide": -0x1000000), so the
// 21H461 kernelcache's 0xfffffff007ade638 (static initializer 0xfffffff0059b52b8) is 0xfffffff006ade638 here. Checked
// in out/cmake/kernel/kernel.macho: the initializer at 0xfffffff0049b52b8 constructs
// OSMetaClass(0xfffffff006ade638, "AppleARMBacklight", &IOService::gMetaClass, 0x370).
// SwitchBacklight names it as its metaclass's superclass; switch_backlight_start() refuses to run unless
// OSMetaClass::getMetaClassWithName("AppleARMBacklight") returns this same address.
.globl __ZN17AppleARMBacklight10gMetaClassE
.set __ZN17AppleARMBacklight10gMetaClassE, 0xfffffff006ade638
