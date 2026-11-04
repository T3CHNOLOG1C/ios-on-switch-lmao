"""XNU: the kernel SDK root, the XNU build (compile + the exact link command), the final link."""

load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "XCODE_EXECUTION", "python", "run_tool", "setting", "workspace_root")

def _kernel_sdkroot_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.out)
    log = ctx.actions.declare_file(ctx.attr.out + ".log")
    py = python(ctx)
    run_tool(
        ctx,
        "sdkroot.py",
        [
            "--xcode-env",
            ctx.file._xcode_env.path,
            "--out",
            out.path,
            "--log",
            log.path,
            "--availability",
            workspace_root(ctx.attr.availability_versions),
            "--libplatform",
            workspace_root(ctx.attr.libplatform),
            "--libdispatch",
            workspace_root(ctx.attr.libdispatch),
            "--xnu",
            workspace_root(ctx.attr.xnu),
            "--python-bin",
            py.interpreter.dirname,
        ],
        depset(ctx.files.availability_versions + ctx.files.libplatform + ctx.files.libdispatch + ctx.files.xnu,
               transitive = [py.files]),
        [out, log],
        "KernelSdkRoot",
        "Assembling the kernel SDK root (XNU installhdrs, libplatform, libdispatch os/, availability.pl)",
    )
    return [DefaultInfo(files = depset([out]))]

kernel_sdkroot = rule(
    implementation = _kernel_sdkroot_impl,
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(default = ".cache/fakeroot"),
        "xnu": attr.label(mandatory = True),
        "libplatform": attr.label(default = "@libplatform//:all"),
        "libdispatch": attr.label(default = "@libdispatch//:all"),
        "availability_versions": attr.label(default = "@availability_versions//:all"),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _kernel_firehose_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.out)
    log = ctx.actions.declare_file(ctx.attr.out + ".log")
    sdkroot = ctx.file.sdkroot
    run_tool(
        ctx,
        "firehose.py",
        [
            "--xcode-env",
            ctx.file._xcode_env.path,
            "--libdispatch",
            workspace_root(ctx.attr.libdispatch),
            "--xnu",
            workspace_root(ctx.attr.xnu),
            "--sdkroot",
            sdkroot.path,
            "--out",
            out.path,
            "--log",
            log.path,
            "--path-root",
            setting(ctx, "_path_root"),
        ],
        depset([sdkroot] + ctx.files.libdispatch + ctx.files.xnu),
        [out, log],
        "KernelFirehose",
        "Building libfirehose_kernel.a (xcodebuild) into the kernel SDK root",
    )
    return [DefaultInfo(files = depset([out]))]

kernel_firehose = rule(
    implementation = _kernel_firehose_impl,
    doc = "The kernel SDK root's headers plus libfirehose_kernel.a: the SDK root XNU and the link use.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(default = ".cache/fakeroot"),
        "sdkroot": attr.label(mandatory = True, allow_single_file = True),
        "xnu": attr.label(mandatory = True),
        "libdispatch": attr.label(default = "@libdispatch//:all"),
    }),
    toolchains = [PY_TOOLCHAIN],
)

XnuInfo = provider(
    doc = "An XNU build: its object root (objects, compile records, exported headers) and its recorded link.",
    fields = ["objroot", "link", "owner_header", "sdkroot"],
)

def _xnu_build_impl(ctx):
    objroot = ctx.actions.declare_directory("build/xnu/ios-obj")
    owner = ctx.actions.declare_file("build/xnu/owner-data-volume.h")
    link = ctx.actions.declare_file("build/xnu/link.json")
    log = ctx.actions.declare_file("build/xnu/build.log")
    run_tool(
        ctx,
        "xnu_build.py",
        [
            "--xcode-env",
            ctx.file._xcode_env.path,
            "--xnu",
            workspace_root(ctx.attr.xnu),
            "--sdkroot",
            ctx.file.sdkroot.path,
            "--options",
            ctx.file.options.path,
            "--tree-root",
            ctx.label.package,
            "--out",
            objroot.path,
            "--owner-header",
            owner.path,
            "--link-json",
            link.path,
            "--log",
            log.path,
            "--path-root",
            setting(ctx, "_path_root"),
            "--bin-dir",
            ctx.bin_dir.path,
            "--build-date",
            setting(ctx, "_build_date"),
            "--builder",
            setting(ctx, "_builder"),
        ],
        depset([ctx.file.sdkroot, ctx.file.options] + ctx.files.xnu + ctx.files.headers),
        [objroot, owner, link, log],
        "XnuBuild",
        "Compiling XNU (make)",
    )
    return [
        DefaultInfo(files = depset([objroot])),
        OutputGroupInfo(log = depset([log])),
        XnuInfo(objroot = objroot, link = link, owner_header = owner, sdkroot = ctx.file.sdkroot),
    ]

xnu_build = rule(
    implementation = _xnu_build_impl,
    doc = "Compiles XNU with make (tools/kernel/xnu_build.py); the link is kernel_link's.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "xnu": attr.label(mandatory = True),
        "sdkroot": attr.label(mandatory = True, allow_single_file = True),
        "options": attr.label(mandatory = True, allow_single_file = [".json"]),
        "headers": attr.label_list(allow_files = True, doc = "Tree files XNU's patched sources include."),
    }),
    toolchains = [PY_TOOLCHAIN],
)
