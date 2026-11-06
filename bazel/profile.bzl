"""//bazel:profile as user-space C, C++ and Objective-C++ see it: SWITCH_DEBUG is 1 in debug and 0 in release.

Kernel code needs no define of its own: XNU's DEVELOPMENT is defined exactly in the debug profile (kernel/rules/defs.bzl
maps debug to XNU DEVELOPMENT and release to RELEASE)."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

SWITCH_DEBUG_DEFINES = {
    "debug": "-DSWITCH_DEBUG=1",
    "release": "-DSWITCH_DEBUG=0",
}

PROFILE_ATTR = {"_profile": attr.label(default = "//bazel:profile")}

def switch_debug_define(ctx):
    """The -D flag for the profile ctx.attr._profile (PROFILE_ATTR) names."""
    return SWITCH_DEBUG_DEFINES[ctx.attr._profile[BuildSettingInfo].value]
