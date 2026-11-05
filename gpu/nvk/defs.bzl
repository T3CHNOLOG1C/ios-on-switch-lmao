"""nvk_dylib: NVK (third_party/mesa, built natively) for one platform, at gpu/nvk/<name>/libvulkan_nouveau.dylib with
its .source beside it (what //gpu takes: the bundle, the seed, the raw pack and the host tools find it there)."""

load("//third_party/mesa/bazel:defs.bzl", "MESA_PLATFORMS", "mesa_platform")

def _nvk_dylib_impl(ctx):
    built = ctx.file.dylib
    dylib = ctx.actions.declare_file(ctx.label.name + "/libvulkan_nouveau.dylib")
    source = ctx.actions.declare_file(ctx.label.name + "/libvulkan_nouveau.dylib.source")

    # A copy, not a symlink: with the disk cache Bazel downloads only top-level outputs, and a symlink to the
    # transitioned build's dylib could be left pointing at a file it never fetched.
    ctx.actions.run(
        executable = "/bin/cp",
        arguments = [built.path, dylib.path],
        inputs = [built],
        outputs = [dylib],
        mnemonic = "NvkDylib",
        progress_message = "Copying %{output}",
    )

    # No -Dnvk-build-id: the pipeline cache UUID hashes the dylib's LC_UUID (tools/gpu/nvk_twin.py reads this line).
    ctx.actions.write(source, "mesa-tree {}\nnvk-build-id \n".format(ctx.attr.mesa_tree))
    return [
        DefaultInfo(files = depset([dylib, source])),
        OutputGroupInfo(dylib = depset([dylib]), source = depset([source])),
    ]

nvk_dylib = rule(
    implementation = _nvk_dylib_impl,
    doc = "libvulkan_nouveau.dylib (+ .source) built for `platform` (ios or macos, the host twin).",
    attrs = {
        "dylib": attr.label(
            mandatory = True,
            allow_single_file = True,
            cfg = mesa_platform,
            doc = "the mesa_dylib target, built in NVK's configuration for `platform`",
        ),
        "mesa_tree": attr.string(mandatory = True, doc = "the .source file's mesa-tree line"),
        "platform": attr.string(mandatory = True, values = MESA_PLATFORMS.keys()),
    },
)
