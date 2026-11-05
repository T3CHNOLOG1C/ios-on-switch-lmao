"""XNU's libfirehose_kernel.a, built into the kernel SDK root (libdispatch's firehose sources, Xcode's xcodebuild).

The SDK root itself is kernel/xnu-<version>:sdkroot (kernel/rules/xnu.bzl), one directory of it the tree's fakeroot_headers."""

load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "run_tool", "setting", "workspace_root")

# XNU's code (not headers, makefiles, MIG .defs or config files): libdispatch's firehose build never reads it, so an edit
# inside an XNU function does not rebuild libfirehose_kernel.a. (installhdrs keeps all of XNU: its setup step checks that
# every source its sanitizer lists name exists.)
_XNU_CODE = ["c", "cpp", "s", "S", "m", "mm"]

def _xnu_dir(target):
    """The XNU tree's exec path: a vendored tree's package (kernel/xnu-<version>), or an external repository's root."""
    return target.label.workspace_root or target.label.package

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
            _xnu_dir(ctx.attr.xnu),
            "--sdkroot",
            sdkroot.path,
            "--out",
            out.path,
            "--log",
            log.path,
            "--path-root",
            setting(ctx, "_path_root"),
        ],
        depset([sdkroot] + ctx.files.libdispatch + [f for f in ctx.files.xnu if f.extension not in _XNU_CODE]),
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
