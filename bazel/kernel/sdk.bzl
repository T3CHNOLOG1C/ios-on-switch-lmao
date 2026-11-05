"""What code linked into the kernel compiles against: the kernel SDK's provider (made by kernel/rules/xnu.bzl xnu_sdk
for each vendored XNU, selected by //kernel:sdk) and generated headers as CcInfo for the kernel's cc targets.

The kernel's compiles run in the kernel configuration (bazel/platforms:ios_kernel_arm64); these rules hand them files
built in the configuration they came from (bazel/platforms/transitions.bzl outer_transition), so each is built once and
every path is the one the link and the earlier flows used.
"""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel/platforms:transitions.bzl", "outer_transition")

KernelSdkInfo = provider(
    doc = "The kernel SDK's files and paths, as the kernel toolchain's flags name them.",
    fields = {
        "headers": "depset of files: XNU's object root's and SDK root's headers, each its own artifact (so .d files " +
                   "prune a compile's inputs to the headers it includes)",
        "obj_dir": "exec path of XNU's object root (kernel/xnu-<version>/obj: EXPORT_HDRS, <CONFIG>_ARM64_BCM2837/...)",
        "sdkroot_dir": "exec path of the kernel SDK root (Kernel.framework, usr/local/include, ...)",
        "objroot": "the object root's spelling in what the build ships (make's ios-obj in the kernel tree's build/)",
        "fakeroot": "the SDK root's spelling in what the build ships (the kernel tree's .cache/fakeroot)",
        "bin_dir": "the bin directory these were built in",
        "xnu_root": "exec path of the XNU source (kernel/xnu-<version>)",
        "tree": "where the kernel tree's sources are (bazel/kernel/layout.bzl TREE_ROOT; \"\" = the repository root)",
        "package": "the package of the kernel's generated files (its build/ holds them)",
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
