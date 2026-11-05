"""xnu_package(): everything one kernel/xnu-<version>/ package declares besides its compile and link rules.

    load("//kernel/rules:defs.bzl", "xnu_package")
    load(":tables/installhdrs.bzl", "INSTALLHDRS")
    ...
    xnu_package(version = "10063.141.1", darwin = "23.6.0", ios_min = "17.0", installhdrs = INSTALLHDRS, ...)

Targets (all public):
  :sdk                      the kernel SDK for drivers (//kernel:sdk selects it by //kernel:xnu_version)
  :obj                      XNU's object root, every file its own artifact (bazel-bin/kernel/xnu-<v>/obj/...), for the
                            profile (//bazel:profile: debug = DEVELOPMENT, release = RELEASE); output groups
                            export_hdrs[_<comp>], gen_<comp>_hdrs, gen_<comp>_srcs, config_makefile_<comp>, as
                            filegroups of the same names
  :sdkroot                  the kernel SDK root (obj/sdkroot/: Kernel.framework, usr/include, usr/local/include, ...)
  :sdkroot_files            :sdkroot and :firehose (libfirehose_kernel.a and its header, in obj/sdkroot/usr/local/)
  :config, :decomment, :setsegname, :kextsymboltool   host tools from SETUP/
  :external_headers         EXTERNAL_HEADERS/**
  :update_tables            bazel run: re-record tables/*.bzl from make (after a makefile, MASTER, conf/files change,
                            or a new Xcode: the tables name its SDKs)
  :tables_test              bazel test (manual): the tables are what make does now
"""

load("@rules_python//python:py_binary.bzl", "py_binary")
load("@rules_python//python:py_test.bzl", "py_test")
load("//bazel/lint:defs.bzl", "lint_files")
load(":xnu.bzl", "xnu_extract", "xnu_host_tool", "xnu_pass", "xnu_sdk")

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

def xnu_sources(name, tags = None):
    """Every file of the XNU tree below this package, across the subpackages (each declares :all)."""
    subs = native.subpackages(include = ["**"], allow_empty = True)
    native.filegroup(
        name = name,
        srcs = native.glob(["**"], exclude = ["**/BUILD.bazel", "**/*.bzl"]) +
               ["//{}/{}:all".format(native.package_name(), s) for s in subs],
        tags = tags,
    )

# The SDK root's other parts each XNU's installhdrs is recorded with (xnu_ops.install_base).
BASE_SOURCES_10063 = {
    "availability": "@availability_versions//:availability",
    "libdispatch": "@libdispatch//:LICENSE",
    "libplatform": "@libplatform//:LICENSE",
}

def xnu_recorder(version, darwin, ios_min, base_sources):
    """:update_tables (bazel run, manual): re-record tables/*.bzl of this kernel/xnu-<version>/ from its make.

    Args:
        version: the XNU version ("10063.141.1"), its make's RC_ProjectSourceVersion.
        darwin: the Darwin version of the OS that XNU shipped in (RC_DARWIN_KERNEL_VERSION).
        ios_min: the iOS deployment target the kernel is built for.
        base_sources: {"availability", "libplatform", "libdispatch"}: a file at the root of each repository.

    Returns:
        struct(args, data): the recorder's arguments and runfiles, for a check that reruns it.
    """
    root = native.package_name()
    args = [
        "--xnu=" + root,
        "--version=" + version,
        "--darwin=" + darwin,
        "--ios-min=" + ios_min,
        "--out={}/tables".format(root),
    ] + ["--{}=$(rootpath {})".format(name, base_sources[name]) for name in ("availability", "libplatform", "libdispatch")]
    data = [base_sources[name] for name in ("availability", "libplatform", "libdispatch")]
    py_binary(
        name = "update_tables",
        srcs = ["//kernel/rules:xnu_record.py", "//kernel/rules:xnu_ops.py"],
        main = "//kernel/rules:xnu_record.py",
        args = args,
        data = data,
        tags = ["manual"],
    )
    return struct(args = args, data = data)

def _xnu_unrecorded_impl(ctx):
    fail("{}: {}".format(ctx.label, ctx.attr.reason))

xnu_unrecorded = rule(
    implementation = _xnu_unrecorded_impl,
    doc = "Stands in for a target of a vendored XNU that cannot be built yet: analysing it fails with the reason.",
    attrs = {"reason": attr.string(mandatory = True)},
)

# Steps make's flow had besides the recorded ones: the empty directories the compiles' -I name (a marker each, as
# make's xnu_build.py made), and the sanitizer ignore list the kernel recipes name (san/Makefile's sed).
_OBJ_EXTRA = [
    {"content": "", "kind": "write", "out": "o/EXPORT_HDRS/libsa/.keep"},
    {"kind": "ignorelist", "out": "o/san/kasan-blacklist-arm64", "srcs": [
        "s/san/memory/kasan-blacklist",
        "s/san/memory/ubsan-blacklist",
        "s/san/memory/kasan-blacklist-arm64",
    ]},
]
_SDKROOT_EXTRA = [
    {"content": "", "kind": "write", "out": "d/usr/local/include/kernel/.keep"},
]

def xnu_package(version, darwin, ios_min, installhdrs, build, kernel_package, extra_headers, firehose):
    """The SDK root, the object root and the SDK of kernel/xnu-<version>, from its recorded tables.

    Args:
        version: the XNU version ("10063.141.1").
        darwin: the Darwin version of the OS that XNU shipped in (xnu_recorder).
        ios_min: the iOS deployment target the kernel is built for (xnu_recorder).
        installhdrs: the installhdrs table.
        build: {"debug": DEVELOPMENT table, "release": RELEASE table}.
        kernel_package: the package of the kernel's other generated files (kernel/rules/layout.bzl KERNEL_PACKAGE).
        extra_headers: the headers XNU's build options put on every kernel compile (-I <tree>/diagnostics/ioswitch).
        firehose: the SDK root with libdispatch's libfirehose_kernel.a installed (a directory): its files go into
            obj/sdkroot beside installhdrs' (:firehose), :sdkroot_files is both.
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

    # libdispatch's firehose library and the header it installs, where make's link and compiles find them.
    xnu_extract(
        name = "firehose",
        src = firehose,
        files = [
            "usr/local/include/kernel/os/firehose_buffer_private.h",
            "usr/local/lib/kernel/libfirehose_kernel.a",
        ],
        out_dir = "obj/sdkroot",
    )
    native.filegroup(name = "sdkroot_files", srcs = [":sdkroot", ":firehose"])

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
        kernel_config = select({
            "//bazel:debug": "DEVELOPMENT",
            "//bazel:release": "RELEASE",
        }),
        kernel_package = kernel_package,
        extra = [extra_headers],
    )

    recorder = xnu_recorder(version = version, darwin = darwin, ios_min = ios_min, base_sources = BASE_SOURCES_10063)

    # Whether the tables are what make does now (a few minutes of make: manual, run it after a makefile change).
    py_test(
        name = "tables_test",
        size = "large",
        srcs = ["//kernel/rules:xnu_record.py", "//kernel/rules:xnu_ops.py"],
        main = "//kernel/rules:xnu_record.py",
        args = recorder.args + ["--check"],
        data = recorder.data + [":srcs"] + native.glob(["tables/*.bzl"]),
        tags = ["manual", "no-remote-exec", "requires-darwin"],
    )
