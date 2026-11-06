"""target_compatible_with for xnu-12377's objects and kernel: built only when the IPSW kind's XNU is 12377 (--config=ios27),
and only for DEVELOPMENT (no RELEASE tables are recorded yet), so the default `bazel build //...` never compiles them."""

def selected_only():
    return select({
        "//bazel:release": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }) + select({
        "//kernel:xnu_12377_121_6": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
