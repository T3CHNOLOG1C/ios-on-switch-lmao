"""The XNU the kernel is built from, as a function of the IPSW kind (ipsw/pins.bzl's "xnu").

--//ipsw:kind (--config=ios17, --config=ios27) is the one setting: the kind's XNU decides kernel/xnu-<version>/, the
XNU_<version> macro and every select() over //kernel:xnu_<version>, so the kernel and the user space cannot disagree.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "config_setting_any", "kinds_where")
load("//ipsw:pins.bzl", "IPSWS")

def _xnu_version_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    xnu = IPSWS[kind].get("xnu")
    if not xnu:
        fail("IPSW kind {} has no \"xnu\" in ipsw/pins.bzl".format(kind))
    return [BuildSettingInfo(value = xnu)]

_xnu_version = rule(
    implementation = _xnu_version_impl,
    doc = "BuildSettingInfo: the selected IPSW kind's XNU version.",
    attrs = {"_kind": attr.label(default = "//ipsw:kind")},
)

def xnu_versions(name):
    """The selected kind's XNU version, and a config_setting xnu_<version> per pinned XNU.

    Args:
        name: the target giving the selected kind's XNU version (BuildSettingInfo).

    Returns:
        The pinned XNU versions, sorted.
    """
    _xnu_version(name = name, visibility = ["//visibility:public"])
    versions = sorted({ipsw["xnu"]: True for ipsw in IPSWS.values() if ipsw.get("xnu")})
    for version in versions:
        config_setting_any(
            name = "xnu_" + version.replace(".", "_"),
            settings = ["//ipsw:" + kind for kind in kinds_where("xnu", version)],
            visibility = ["//visibility:public"],
        )
    return versions
