"""What code linked into the kernel compiles against: XNU's headers (exported, generated into its object directories,
and in the patched source), the kernel SDK root, and generated headers, as CcInfo for the kernel's cc targets.

The kernel's compiles run in the kernel configuration (bazel/platforms:ios_kernel_arm64); these rules hand them files
built in the configuration they came from (bazel/platforms/transitions.bzl outer_transition), so XNU is built once and
every path is the one the link and the earlier flows used.

xnu_headers copies only the headers (and the sanitizer lists the recipes name) out of XNU's object root: an XNU change
that leaves every header alone leaves this tree's digest alone, so no driver recompiles (Bazel prunes on unchanged
outputs).
"""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel/platforms:transitions.bzl", "outer_transition")
load(":xnu.bzl", "XnuInfo")

KernelSdkInfo = provider(
    doc = "The kernel SDK's paths, as the kernel toolchain's flags name them.",
    fields = {
        "headers": "tree artifact: XNU's object root, headers only (build/xnu/ios-hdrs)",
        "objroot": "exec path of XNU's object root (what the strings in objects name)",
        "fakeroot": "tree artifact: the kernel SDK root (.cache/fakeroot)",
        "bin_dir": "the bin directory these were built in",
        "xnu_root": "exec path of the patched XNU source (external/<repo>)",
        "tree": "the kernel tree's package path",
    },
)

_HEADER_EXTENSIONS = ("h", "hpp", "inc", "def")

def _xnu_headers_impl(ctx):
    xnu = ctx.attr.xnu[XnuInfo]
    out = ctx.actions.declare_directory(ctx.attr.out)
    ctx.actions.run_shell(
        inputs = [xnu.objroot],
        outputs = [out],
        arguments = [xnu.objroot.path, out.path],
        # Headers, directory markers and the sanitizer ignore lists; the lists' @EXECROOT@ (xnu_build.py's stand-in for
        # the execution root) becomes the relative root the compiles see.
        command = """set -e
dst=$PWD/$2
cd "$1"
# The sandbox stages a tree artifact's files as links: follow them (find -L, cpio -L copies the contents).
/usr/bin/find -L . -type f \\( -name '*.h' -o -name '.keep' -o -path './san/*' \\) -print0 | /usr/bin/cpio -0pdL --quiet "$dst"
/usr/bin/sed -i '' 's#@EXECROOT@/##g' "$dst"/san/*
""",
        mnemonic = "XnuHeaders",
        progress_message = "Copying XNU's headers out of its object root",
    )
    return [
        DefaultInfo(files = depset([out])),
        KernelSdkInfo(
            headers = out,
            objroot = xnu.objroot.path,
            fakeroot = xnu.sdkroot,
            bin_dir = ctx.bin_dir.path,
            xnu_root = ctx.attr.xnu_source.label.workspace_root,
            tree = ctx.label.package,
        ),
    ]

xnu_headers = rule(
    implementation = _xnu_headers_impl,
    doc = "XNU's object root without its objects: what the kernel's compiles include from it.",
    attrs = {
        "xnu": attr.label(mandatory = True, providers = [XnuInfo]),
        "xnu_source": attr.label(mandatory = True, doc = "the patched XNU source (its repository)"),
        "out": attr.string(default = "build/xnu/ios-hdrs"),
    },
)

def _one(target):
    """A label attribute through a 1:1 transition (a list of one target in some Bazel versions)."""
    return target[0] if type(target) == "list" else target

def _kernel_sdk_impl(ctx):
    sdk = _one(ctx.attr.headers)[KernelSdkInfo]
    source = [f for f in ctx.files.xnu_source if f.extension in _HEADER_EXTENSIONS]
    context = cc_common.create_compilation_context(headers = depset([sdk.headers, sdk.fakeroot] + source + ctx.files.extra))

    # Make variables for the drivers' flags (kernel_driver's {build}, {xnu}): the outer configuration's paths.
    variables = platform_common.TemplateVariableInfo({
        "KERNEL_BUILD": "{}/{}/build".format(sdk.bin_dir, sdk.tree),
        "KERNEL_XNU": sdk.xnu_root,
    })
    return [
        sdk,
        variables,
        CcInfo(compilation_context = context),
        DefaultInfo(files = depset([sdk.headers, sdk.fakeroot])),
    ]

kernel_sdk = rule(
    implementation = _kernel_sdk_impl,
    doc = "The kernel SDK as CcInfo for the kernel configuration: xnu_headers (from the outer configuration) and the " +
          "patched XNU source's headers. Every kernel cc target depends on it; the kernel toolchain reads its paths.",
    attrs = {
        "headers": attr.label(mandatory = True, providers = [KernelSdkInfo], cfg = outer_transition),
        "xnu_source": attr.label(mandatory = True, allow_files = True),
        "extra": attr.label_list(
            allow_files = True,
            doc = "headers XNU's own flags put on every kernel compile (its CFLAGS_EXTRA -I <tree>/diagnostics/ioswitch); " +
                  "files, not cc targets (the kernel toolchain depends on this rule)",
        ),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def _outer_headers_impl(ctx):
    files = depset(transitive = [t[DefaultInfo].files for t in ctx.attr.srcs])
    return [
        DefaultInfo(files = files),
        CcInfo(compilation_context = cc_common.create_compilation_context(headers = files)),
    ]

outer_headers = rule(
    implementation = _outer_headers_impl,
    doc = "Generated headers (and .incbin inputs) built in the outer configuration, as CcInfo headers for kernel cc " +
          "targets: one build of each, at the path the earlier flows used.",
    attrs = {
        "srcs": attr.label_list(allow_files = True, cfg = outer_transition),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)
