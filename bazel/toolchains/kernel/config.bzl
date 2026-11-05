"""The kernel's C/C++/assembly toolchain (https://bazel.build/tutorials/ccp-toolchain-config): Xcode's Apple clang (the
build's one host input; XNU needs Apple clang's builtins and the kernel must stay bit-identical) with exactly the flags
XNU's own compile records give code linked into the kernel (recipes.bzl), as features:

  kernel_<recipe>           the recipe's flags: kernel_iokit_cxx (C++, default) and kernel_osfmk_c (C, default), or one of
                            kernel_iokit_cxx_generic / kernel_bsd_c / kernel_mockfs_c / kernel_freestanding_c /
                            kernel_plain_c chosen by a target's features (kernel_driver does that); recipes of one language
                            exclude each other (provides)
  apple_clang_only          (default) the flags only Apple clang takes or only code generation needs: off for clang-tidy
  kernel_warnings           (default) the drivers' -W set on C++ (bazel/kernel/kernel.bzl DRIVER_WARNINGS)
  kernel_plain_asm          (default) .S/.s: Xcode clang for iOS 17 against the iPhoneOS SDK, nothing from XNU

Every path in a flag is execution-root relative; -fmacro-prefix-map/-fdebug-prefix-map make the strings that reach the
objects (__FILE__, DWARF) the ones the CMake flow and the earlier Bazel flow produced from absolute paths: the patched
XNU as <root>/out/native-a57/root-v1/tree/.cache/xnu-work, build outputs as <root>/<package path>, sources as
<root>/<path>. (Apple clang resolves macro maps by the most specific prefix and debug maps by the last match.)
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "action_config",
    "env_entry",
    "env_set",
    "feature",
    "feature_set",
    "flag_group",
    "flag_set",
    "tool",
    "variable_with_value",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@xcode//:defs.bzl", "DEVELOPER_DIR", "IPHONEOS_SDK", "XCODE_TOOLCHAIN")
load("//bazel/kernel:layout.bzl", "compat_prefix_maps")
load("//bazel/kernel:sdk.bzl", "KernelSdkInfo")
load(":recipes.bzl", "RECIPES")

PATH_ROOT = "/ioswitch"

# Where the CMake flow kept the patched XNU checkout: the XNU source's spelling in every __FILE__.
XNU_SPELLING = PATH_ROOT + "/out/native-a57/root-v1/tree/.cache/xnu-work"

# The XNU source's spelling in debug information: where the @xnu repository (the patched tarball) had it.
XNU_REPOSITORY = "external/+http_archive+xnu"

# cmake/Drivers.cmake IOSWITCH_DRIVER_WARNINGS: what every IOKit C++ driver adds to the kernel's flags.
DRIVER_WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-c99-designator",
    "-Wno-reorder-init-list",
    "-Wno-deprecated-enum-enum-conversion",
    "-Wno-unnecessary-virtual-specifier",
]

_CXX = [ACTION_NAMES.cpp_compile]
_C = [ACTION_NAMES.c_compile]
_ASM = [ACTION_NAMES.assemble, ACTION_NAMES.preprocess_assemble]
_COMPILE = _C + _CXX + _ASM

# Which language each recipe compiles; the defaults.
_LANGUAGE = {
    "iokit_cxx": "cxx",
    "iokit_cxx_generic": "cxx",
    "osfmk_c": "c",
    "bsd_c": "c",
    "mockfs_c": "c",
    "freestanding_c": "c",
}
_DEFAULTS = ["iokit_cxx", "osfmk_c"]

def _flags(values):
    return [flag_group(flags = values)] if values else []

def _impl(ctx):
    sdk = ctx.attr.sdk[KernelSdkInfo]
    places = {
        "{xnu_obj}": sdk.obj_dir,
        "{fakeroot}": sdk.sdkroot_dir,
        "{xnu}": sdk.xnu_root,
        # "{tree}/x" is x below the kernel tree (the repository root since the layout move).
        "{tree}/": sdk.tree + "/" if sdk.tree else "",
    }

    def expand(values):
        out = []
        for value in values:
            for key, path in places.items():
                value = value.replace(key, path)
            out.append(value)
        return out

    # XNU's object root and SDK root (each file its own artifact under kernel/xnu-<version>/obj) keep the spellings
    # they had as make's trees in the kernel package (sdk.objroot, sdk.fakeroot), and XNU's source the spellings it had
    # as the @xnu repository: XNU_SPELLING in __FILE__, external/+http_archive+xnu in debug information.
    objroot_relative = sdk.objroot[len(sdk.bin_dir) + 1:]
    fakeroot_relative = sdk.fakeroot[len(sdk.bin_dir) + 1:]
    macro_maps = ["-fmacro-prefix-map={}/={}/{}/".format(d, PATH_ROOT, d) for d in ctx.attr.source_roots + ["external"]] + [
        "-fmacro-prefix-map={}={}".format(sdk.xnu_root, XNU_SPELLING),
        "-fmacro-prefix-map={}={}".format(sdk.bin_dir, PATH_ROOT),
        "-fmacro-prefix-map={}={}/{}".format(sdk.obj_dir, PATH_ROOT, objroot_relative),
        "-fmacro-prefix-map={}={}/{}".format(sdk.sdkroot_dir, PATH_ROOT, fakeroot_relative),
    ]
    debug_maps = ["-fdebug-prefix-map={}/={}/{}/".format(d, PATH_ROOT, d) for d in ctx.attr.source_roots + ["external", "bazel-out"]]
    compat_macro, compat_debug = compat_prefix_maps(sdk.bin_dir)

    # Debug maps go by the last match: the SDK root (inside the object root) after it, XNU's source after the source
    # roots.
    debug_maps += compat_debug + [
        "-fdebug-prefix-map={}={}/{}".format(sdk.obj_dir, PATH_ROOT, sdk.objroot),
        "-fdebug-prefix-map={}={}/{}".format(sdk.sdkroot_dir, PATH_ROOT, sdk.fakeroot),
        "-fdebug-prefix-map={}={}/{}".format(sdk.xnu_root, PATH_ROOT, XNU_REPOSITORY),
    ]
    maps = macro_maps + compat_macro + debug_maps

    env = [env_set(
        actions = _COMPILE + [ACTION_NAMES.cpp_link_static_library],
        env_entries = [
            env_entry("PATH", "/usr/bin:/bin:/usr/sbin:/sbin"),
            env_entry("DEVELOPER_DIR", DEVELOPER_DIR),
            env_entry("LC_ALL", "C"),
            env_entry("LANG", "C"),
            env_entry("TZ", "UTC"),
            env_entry("ZERO_AR_DATE", "1"),
        ],
    )]

    features = [
        feature(name = "no_legacy_features", enabled = True),
        feature(name = "apple_clang_only", enabled = True),
        feature(name = "kernel_env", enabled = True, env_sets = env),
    ]
    for name, recipe in RECIPES.items():
        language = _LANGUAGE[name]
        actions = _CXX if language == "cxx" else _C
        features.append(feature(
            name = "kernel_" + name,
            enabled = name in _DEFAULTS,
            provides = ["kernel_{}_recipe".format(language)],
            flag_sets = [flag_set(actions = actions, flag_groups = _flags(
                expand(recipe.flags) + maps + ["-fdebug-compilation-dir=" + recipe.debug_dir],
            ))],
        ))
        features.append(feature(
            name = "kernel_{}_apple".format(name),
            enabled = True,
            requires = [feature_set(features = ["kernel_" + name, "apple_clang_only"])],
            flag_sets = [flag_set(actions = actions, flag_groups = _flags(expand(recipe.apple)))],
        ))

    # cmake/XnuRecipes.cmake ioswitch_compile_command: `xcrun --sdk iphoneos clang -arch arm64 -mkernel ...` (link stubs).
    features.append(feature(
        name = "kernel_plain_c",
        provides = ["kernel_c_recipe"],
        flag_sets = [flag_set(actions = _C, flag_groups = _flags(["-mkernel", "-miphoneos-version-min=17.0", "-Os"] + maps))],
        env_sets = [env_set(actions = _C, env_entries = [env_entry("SDKROOT", IPHONEOS_SDK)])],
    ))
    features.append(feature(
        name = "kernel_plain_c_apple",
        enabled = True,
        requires = [feature_set(features = ["kernel_plain_c", "apple_clang_only"])],
        flag_sets = [flag_set(actions = _C, flag_groups = _flags(["-arch", "arm64"]))],
    ))

    # The XNU the kernel is built from (//kernel:xnu_version) as a macro, for code that differs per version:
    # #if XNU_10063_141_1.
    features.append(feature(
        name = "xnu_version_macro",
        enabled = True,
        flag_sets = [flag_set(actions = _COMPILE, flag_groups = _flags(
            ["-DXNU_{}=1".format(ctx.attr._xnu_version[BuildSettingInfo].value.replace(".", "_"))],
        ))],
    ))

    # PLAIN_ASM: bindings, firmware .incbin units.
    features.append(feature(
        name = "kernel_plain_asm",
        enabled = True,
        flag_sets = [flag_set(actions = _ASM, flag_groups = _flags(
            ["-target", "arm64-apple-ios17.0", "-isysroot", IPHONEOS_SDK, "-Werror"] + maps,
        ))],
    ))

    features += [
        feature(
            name = "kernel_warnings",
            enabled = True,
            flag_sets = [flag_set(actions = _CXX, flag_groups = _flags(DRIVER_WARNINGS))],
        ),
        feature(
            name = "user_compile_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["%{user_compile_flags}"],
                iterate_over = "user_compile_flags",
                expand_if_available = "user_compile_flags",
            )])],
        ),
        feature(
            name = "dependency_file",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-MD", "-MF", "%{dependency_file}"],
                expand_if_available = "dependency_file",
            )])],
        ),
        feature(
            name = "compiler_output_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-o", "%{output_file}"],
                expand_if_available = "output_file",
            )])],
        ),
        feature(
            name = "compiler_input_flags",
            enabled = True,
            flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
                flags = ["-c", "%{source_file}"],
                expand_if_available = "source_file",
            )])],
        ),
        feature(
            name = "archiver_flags",
            enabled = True,
            flag_sets = [flag_set(actions = [ACTION_NAMES.cpp_link_static_library], flag_groups = [
                flag_group(flags = ["rcs", "%{output_execpath}"], expand_if_available = "output_execpath"),
                flag_group(
                    iterate_over = "libraries_to_link",
                    expand_if_available = "libraries_to_link",
                    flag_groups = [
                        flag_group(
                            flags = ["%{libraries_to_link.name}"],
                            expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file"),
                        ),
                        flag_group(
                            flags = ["%{libraries_to_link.object_files}"],
                            iterate_over = "libraries_to_link.object_files",
                            expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file_group"),
                        ),
                    ],
                ),
            ])],
        ),
    ]

    clang = tool(path = XCODE_TOOLCHAIN + "/clang")
    action_configs = [
        action_config(action_name = name, enabled = True, tools = [clang])
        for name in _COMPILE
    ] + [
        action_config(
            action_name = ACTION_NAMES.cpp_link_static_library,
            enabled = True,
            tools = [tool(path = XCODE_TOOLCHAIN + "/ar")],
        ),
    ]
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "ioswitch-kernel",
        host_system_name = "aarch64-apple-darwin",
        target_system_name = "arm64-apple-ios17.0-kernel",
        target_cpu = "arm64",
        target_libc = "none",
        compiler = "apple-clang",
        abi_version = "none",
        abi_libc_version = "none",
        features = features,
        action_configs = action_configs,
        # Xcode (clang's resource headers, the SDKs): absolute, outside the execution root, declared by @xcode//:identity.
        cxx_builtin_include_directories = [DEVELOPER_DIR],
    )

kernel_cc_toolchain_config = rule(
    implementation = _impl,
    doc = "The kernel's CcToolchainConfigInfo: Apple clang with XNU's recipes as features.",
    attrs = {
        "sdk": attr.label(mandatory = True, providers = [KernelSdkInfo], doc = "the kernel SDK (//kernel:sdk)"),
        "_xnu_version": attr.label(default = "//kernel:xnu_version"),
        "source_roots": attr.string_list(
            mandatory = True,
            doc = "the main repository's top-level directories kernel sources live in (their strings get <root>/)",
        ),
    },
    provides = [CcToolchainConfigInfo],
)
