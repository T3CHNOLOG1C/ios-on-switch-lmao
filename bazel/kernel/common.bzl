"""Shared plumbing for the kernel rules: the hermetic Python that runs every tool, the Xcode host input, the path
settings that keep sandbox paths out of the outputs."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

PY_TOOLCHAIN = "@rules_python//python:toolchain_type"

# Every action that runs an Xcode tool: macOS only, never on a remote executor (Xcode cannot be shipped there), but
# its results may be cached remotely (the action key includes @xcode//:identity).
XCODE_EXECUTION = {"requires-darwin": "", "no-remote-exec": ""}

def python(ctx):
    runtime = ctx.toolchains[PY_TOOLCHAIN].py3_runtime
    return struct(interpreter = runtime.interpreter, files = runtime.files)

def setting(ctx, attr):
    return getattr(ctx.attr, attr)[BuildSettingInfo].value

def workspace_root(target):
    """`external/<canonical repo name>` of a target in an external repository ("" for the main repository)."""
    return target.label.workspace_root

KERNEL_TOOL_ATTRS = {
    "_tools": attr.label(default = "//tools/kernel:sources"),
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    "_path_root": attr.label(default = "//bazel/kernel:path_root"),
    "_build_date": attr.label(default = "//bazel/kernel:build_date"),
    "_builder": attr.label(default = "//bazel/kernel:builder"),
}

def tool_inputs(ctx):
    """Files every kernel tool action needs: the tools, the Python runtime, the Xcode identity and env."""
    return depset(
        ctx.files._tools + [ctx.file._xcode_env, ctx.file._xcode_identity],
        transitive = [python(ctx).files],
    )

def run_tool(ctx, tool, arguments, inputs, outputs, mnemonic, progress, execution_requirements = None, env = None):
    """Runs tools/kernel/<tool> (or a script File) on the hermetic interpreter (-I: no site, no cwd on sys.path)."""
    py = python(ctx)
    if type(tool) == "File":
        script = [tool]
    else:
        script = [f for f in ctx.files._tools if f.basename == tool]
    if len(script) != 1:
        fail("no tools/kernel/" + str(tool))
    ctx.actions.run(
        executable = py.interpreter,
        arguments = ["-I", "-B", script[0].path] + arguments,
        inputs = depset(script, transitive = [inputs, tool_inputs(ctx)]),
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = progress,
        execution_requirements = execution_requirements or XCODE_EXECUTION,
        env = env or {},
    )
