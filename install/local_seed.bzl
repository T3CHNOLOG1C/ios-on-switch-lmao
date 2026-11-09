"""@switch_shader_seed: a shader seed built on another Mac (//gpu/seed:tree and its size file), for one that cannot
build its own: --repo_env=SWITCH_SHADER_SEED_DIR=<a directory holding tree/ and tree.size.json>, then
--//install:shader_seed=@switch_shader_seed//:tree --//install:shader_seed_size=@switch_shader_seed//:size.
"""

_BUILD = """load(":copy.bzl", "copy_tree")

package(default_visibility = ["//visibility:public"])

copy_tree(
    name = "tree",
    src = "seed/tree",
)

filegroup(
    name = "size",
    srcs = ["seed/tree.size.json"],
)
"""

# A tree artifact, as //gpu/seed:tree is: image_inputs wants one directory, and a source directory stands in for one
# only by accident.
_COPY = """def _impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run_shell(
        inputs = [ctx.file.src],
        outputs = [out],
        command = "cp -R \\"$1\\"/. \\"$2\\"",
        arguments = [ctx.file.src.path, out.path],
        mnemonic = "CopySeedTree",
    )
    return [DefaultInfo(files = depset([out]))]

copy_tree = rule(implementation = _impl, attrs = {"src": attr.label(allow_single_file = True)})
"""

_MISSING = """def _impl(ctx):
    fail({message})

missing = rule(implementation = _impl)
"""

_MISSING_BUILD = """load(":missing.bzl", "missing")

package(default_visibility = ["//visibility:public"])

[missing(name = name) for name in ("tree", "size")]
"""

def _seed_impl(rctx):
    local = rctx.getenv("SWITCH_SHADER_SEED_DIR")
    if local and rctx.path(local + "/tree").exists and rctx.path(local + "/tree.size.json").exists:
        # A copy, not a symlink: the sandbox shows actions nothing outside the execroot.
        result = rctx.execute(["cp", "-R", local + "/", "seed"])
        if result.return_code:
            fail("copying {}: {}".format(local, result.stderr))
        rctx.file("copy.bzl", _COPY)
        rctx.file("BUILD.bazel", _BUILD)
        return
    message = "SWITCH_SHADER_SEED_DIR is {}: point --repo_env=SWITCH_SHADER_SEED_DIR at a directory holding tree/ and tree.size.json".format(
        repr(local) if local else "unset",
    )
    rctx.file("missing.bzl", _MISSING.format(message = repr(message)))
    rctx.file("BUILD.bazel", _MISSING_BUILD)

switch_shader_seed = repository_rule(
    implementation = _seed_impl,
    environ = ["SWITCH_SHADER_SEED_DIR"],
)

def _extension_impl(_mctx):
    switch_shader_seed(name = "switch_shader_seed")

shader_seed = module_extension(implementation = _extension_impl)
