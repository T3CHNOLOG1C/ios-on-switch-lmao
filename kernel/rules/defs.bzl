"""xnu_package(): everything one kernel/xnu-<version>/ package declares besides its compile and link rules.

    load("//kernel/rules:defs.bzl", "xnu_package")
    load(":tables/installhdrs.bzl", "INSTALLHDRS")
    ...
    xnu_package(version = "10063.141.1", installhdrs = INSTALLHDRS, build = {"debug": ..., "release": ...})

Targets (all public):
  :sdk                      the kernel SDK for drivers (//kernel:sdk selects it by //kernel:xnu_version)
  :obj                      XNU's object root, every file its own artifact (bazel-bin/kernel/xnu-<v>/obj/...), for the
                            profile (//bazel:profile: debug = DEVELOPMENT, release = RELEASE); output groups
                            export_hdrs[_<comp>], gen_<comp>_hdrs, gen_<comp>_srcs, config_makefile_<comp>, as
                            filegroups of the same names
  :sdkroot                  the kernel SDK root (obj/sdkroot/: Kernel.framework, usr/include, usr/local/include, ...)
  :config, :decomment, :setsegname, :kextsymboltool   host tools from SETUP/
  :external_headers         EXTERNAL_HEADERS/**
  :update_tables            bazel run: re-record tables/*.bzl from make (after a makefile, MASTER, conf/files change)
"""

load("@rules_python//python:py_binary.bzl", "py_binary")
load("//bazel/lint:defs.bzl", "lint_files")
load(":xnu.bzl", "xnu_host_tool", "xnu_pass", "xnu_sdk")

# XNU's own files keep XNU's style: lint_files() in the vendored tree checks only our Starlark.
XNU_FOREIGN = [
    "**/*.py",
    "**/*.c",
    "**/*.cc",
    "**/*.cpp",
    "**/*.h",
    "**/*.hpp",
    "**/*.inc",
    "**/*.m",
    "**/*.mm",
    "**/*.rs",
    "**/*.sh",
    "**/*.md",
    "**/*.json",
    "**/*.toml",
    "**/.clang-format",
    "**/.clang-tidy",
]

# The components r18-xnu-objs' compile rules use the generated files of.
COMPONENTS = ["osfmk", "bsd", "iokit", "libkern", "libsa", "pexpert", "san", "security"]

def xnu_lint_files():
    """lint_files() for a package inside a vendored XNU tree."""
    lint_files(exclude = XNU_FOREIGN + ["tables/*.bzl"])

def xnu_sources(name):
    """Every file of the XNU tree below this package, across the subpackages (each declares :all)."""
    subs = native.subpackages(include = ["**"], allow_empty = True)
    native.filegroup(
        name = name,
        srcs = native.glob(["**"], exclude = ["**/BUILD.bazel", "**/*.bzl"]) +
               ["//{}/{}:all".format(native.package_name(), s) for s in subs],
    )

# Steps make's flow had besides the recorded ones: the empty directories the compiles' -I name (a marker each, as
# tools/kernel/xnu_build.py made), and the sanitizer ignore list the kernel recipes name (san/Makefile's sed).
_OBJ_EXTRA = [
    {"kind": "write", "out": "o/EXPORT_HDRS/libsa/.keep", "content": ""},
    {"kind": "ignorelist", "out": "o/san/kasan-blacklist-arm64", "srcs": [
        "s/san/memory/kasan-blacklist",
        "s/san/memory/ubsan-blacklist",
        "s/san/memory/kasan-blacklist-arm64",
    ]},
]
_SDKROOT_EXTRA = [
    {"kind": "write", "out": "d/usr/local/include/kernel/.keep", "content": ""},
]

def xnu_package(version, installhdrs, build, kernel_package, extra_headers):
    """The SDK root, the object root and the SDK of kernel/xnu-<version>, from its recorded tables.

    Args:
        version: the XNU version ("10063.141.1").
        installhdrs: the installhdrs table.
        build: {"debug": DEVELOPMENT table, "release": RELEASE table}.
        kernel_package: the package of the kernel's other generated files (bazel/kernel/layout.bzl KERNEL_PACKAGE).
        extra_headers: the headers XNU's build options put on every kernel compile (-I <tree>/diagnostics/ioswitch).
    """
    root = native.package_name()
    xnu_sources(name = "srcs")
    native.filegroup(name = "external_headers", srcs = native.glob(["EXTERNAL_HEADERS/**"]))

    xnu_host_tool(
        name = "config",
        srcs = native.glob(["SETUP/config/*.c"]),
        hdrs = ["SETUP/config/config.h"],
        yacc = "SETUP/config/parser.y",
        lex = "SETUP/config/lexer.l",
        copts = ["-DYY_NO_INPUT", "-w"],
    )
    for tool, linkopts in (("decomment", []), ("setsegname", []), ("kextsymboltool", ["-lstdc++"]), ("installfile", [])):
        xnu_host_tool(
            name = tool,
            srcs = ["SETUP/{}/{}.c".format(tool, tool)],
            copts = ["-w"],
            linkopts = linkopts,
        )

    xnu_pass(
        name = "sdkroot",
        table = json.encode({"flags": installhdrs["flags"], "steps": installhdrs["steps"] + _SDKROOT_EXTRA}),
        srcs = [":srcs"],
        xnu_root = root,
        obj_dir = "sdk-obj",
        dst_dir = "obj/sdkroot",
        config = ":config",
        decomment = ":decomment",
    )
    xnu_pass(
        name = "obj",
        table = select({
            "//bazel:debug": json.encode({"flags": build["debug"]["flags"], "steps": build["debug"]["steps"] + _OBJ_EXTRA}),
            "//bazel:release": json.encode({"flags": build["release"]["flags"], "steps": build["release"]["steps"] + _OBJ_EXTRA}),
        }),
        srcs = [":srcs"],
        xnu_root = root,
        obj_dir = "obj",
        sdkroot = ":sdkroot",
        config = ":config",
        decomment = ":decomment",
    )
    groups = ["export_hdrs", "sdk_headers"]
    for comp in COMPONENTS:
        groups += ["export_hdrs_" + comp, "gen_{}_hdrs".format(comp), "gen_{}_srcs".format(comp), "config_makefile_" + comp]
    for group in groups:
        native.filegroup(name = group, srcs = [":obj"], output_group = group)

    xnu_sdk(
        name = "sdk",
        obj = ":obj",
        sdkroot = ":sdkroot",
        xnu_source = [":srcs"],
        xnu_root = root,
        package_dir = root,
        obj_dir = "obj",
        kernel_package = kernel_package,
        extra = [extra_headers],
    )

    record_args = [
        "--xnu=" + root,
        "--version=" + version,
        "--out={}/tables".format(root),
        # A file at each repository's root (the recorder takes its directory).
        "--availability=$(rootpath @availability_versions//:availability)",
        "--libplatform=$(rootpath @libplatform//:LICENSE)",
        "--libdispatch=$(rootpath @libdispatch//:LICENSE)",
    ]
    record_data = ["@availability_versions//:availability", "@libplatform//:LICENSE", "@libdispatch//:LICENSE"]
    py_binary(
        name = "update_tables",
        srcs = ["//kernel/rules:xnu_record.py", "//kernel/rules:xnu_ops.py"],
        main = "//kernel/rules:xnu_record.py",
        args = record_args,
        data = record_data,
        tags = ["manual"],
    )
