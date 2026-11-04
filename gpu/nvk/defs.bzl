"""NVK under Bazel: the tools the Mesa build runs (bindgen, cbindgen, from their own Cargo.locks) and the meson build
itself (gpu/nvk/build_nvk.py), for iOS and as the macOS host twin."""

load("@rules_python//python:defs.bzl", "PyInfo")
load("//bazel/kernel:common.bzl", "XCODE_EXECUTION")

def _cargo_tool_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    rustc = ctx.file._rustc
    cargo = ctx.file._cargo
    src = ctx.file.manifest.dirname
    vendor = src.rpartition("/")[0] + "/vendor"
    ctx.actions.run_shell(
        command = """\
set -e
here=$PWD
export CARGO_HOME=$here/{out}.cargo-home RUSTC=$here/{rustc} PATH=/usr/bin:/bin
export DEVELOPER_DIR=$(sed -n 's/^DEVELOPER_DIR=//p' {xcode_env})
cd {src}
$here/{cargo} build --release --offline --locked --bin {bin} --target-dir $here/{out}.target \\
  --config 'source.crates-io.replace-with="vendored"' --config "source.vendored.directory=\\"$here/{vendor}\\""
cp $here/{out}.target/release/{bin} $here/{out}
rm -rf $here/{out}.target $here/{out}.cargo-home
""".format(out = out.path, rustc = rustc.path, cargo = cargo.path, src = src, vendor = vendor, bin = ctx.attr.bin,
           xcode_env = ctx.file._xcode_env.path),
        inputs = ctx.files.srcs + ctx.files._rust + [ctx.file._xcode_env, ctx.file._xcode_identity],
        outputs = [out],
        mnemonic = "CargoTool",
        progress_message = "Building {} with cargo (offline, its own Cargo.lock)".format(ctx.attr.bin),
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]), executable = out)]

cargo_tool = rule(
    implementation = _cargo_tool_impl,
    doc = "A crates.io binary built by Rust 1.96.1's cargo from a vendored source repository (gpu/nvk/repos.bzl).",
    executable = True,
    attrs = {
        "srcs": attr.label(mandatory = True, doc = "the repository's :all"),
        "manifest": attr.label(mandatory = True, allow_single_file = True, doc = "its src/Cargo.toml"),
        "bin": attr.string(mandatory = True),
        "_rust": attr.label(default = "@nvk_rust//:all"),
        "_rustc": attr.label(default = "@nvk_rust//:sysroot/bin/rustc", allow_single_file = True),
        "_cargo": attr.label(default = "@nvk_rust//:sysroot/bin/cargo", allow_single_file = True),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)

def _python_path(target):
    """The import roots of a py_library's transitive closure, as execroot paths (external/<repo>/site-packages...)."""
    roots = []
    for imp in target[PyInfo].imports.to_list():
        root = imp if imp.startswith("external/") else "external/" + imp
        if root not in roots:
            roots.append(root)
    return roots

def _nvk_dylib_impl(ctx):
    dylib = ctx.actions.declare_file(ctx.label.name + "/libvulkan_nouveau.dylib")
    source = ctx.actions.declare_file(ctx.label.name + "/libvulkan_nouveau.dylib.source")
    sysroot = ctx.file._rustc.dirname.rpartition("/")[0]
    args = ctx.actions.args()
    args.add("--platform", ctx.attr.platform)
    args.add("--mesa", ctx.file._mesa)
    args.add_all(ctx.files.patches, before_each = "--patch")
    args.add("--wraps", ctx.file._wraps_marker.dirname.rpartition("/")[0])
    args.add("--abi-header", ctx.file.abi_header)
    python_inputs = []
    for dep in ctx.attr._python_deps:
        for root in _python_path(dep):
            args.add("--python-path", root)
        python_inputs.append(dep[PyInfo].transitive_sources)
        python_inputs.append(dep[DefaultInfo].default_runfiles.files)
    args.add("--ninja", ctx.file._ninja)
    args.add("--rust-sysroot", sysroot)
    args.add("--bindgen", ctx.executable._bindgen)
    args.add("--cbindgen", ctx.executable._cbindgen)
    args.add("--libclang", ctx.file._libclang.dirname.rpartition("/")[0])
    args.add("--pinned", ctx.file._pinned_inputs.dirname)
    args.add("--relink", ctx.executable._relink)
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--git-sha1", ctx.attr.git_sha1)
    args.add("--build-id", ctx.attr.build_id)
    if ctx.attr.expect_tree:
        args.add("--expect-tree", ctx.attr.expect_tree)
    if ctx.attr.expect_sha256:
        args.add("--expect-sha256", ctx.attr.expect_sha256)
    args.add("--rust-src-prefix", ctx.attr.rust_src_prefix)
    args.add("--out", dylib)
    args.add("--out-source", source)
    ctx.actions.run(
        executable = ctx.executable._build,
        arguments = [args],
        inputs = depset(
            [ctx.file._mesa, ctx.file.abi_header, ctx.file._ninja, ctx.file._xcode_env, ctx.file._xcode_identity] +
            ctx.files.patches + ctx.files._wraps + ctx.files._rust + ctx.files._libclang_files + ctx.files._pinned,
            transitive = python_inputs,
        ),
        tools = [
            ctx.attr._bindgen[DefaultInfo].files_to_run,
            ctx.attr._cbindgen[DefaultInfo].files_to_run,
            ctx.attr._relink[DefaultInfo].files_to_run,
        ],
        outputs = [dylib, source],
        mnemonic = "NvkMeson",
        progress_message = "Building NVK for {} (Mesa + {} patches, meson + ninja)".format(
            ctx.attr.platform, len(ctx.files.patches)),
        # The compile runs xcrun (Xcode) and the meson build is one action of ~1500 compiles.
        execution_requirements = XCODE_EXECUTION,
    )
    return [
        DefaultInfo(files = depset([dylib, source])),
        OutputGroupInfo(dylib = depset([dylib]), source = depset([source])),
    ]

nvk_dylib = rule(
    implementation = _nvk_dylib_impl,
    doc = "libvulkan_nouveau.dylib (+ .source) from Mesa at the pinned commit and a patch series.",
    attrs = {
        "platform": attr.string(mandatory = True, values = ["ios", "macos"]),
        "patches": attr.label_list(allow_files = [".patch"], doc = "applied in this order"),
        "abi_header": attr.label(mandatory = True, allow_single_file = True, doc = "TegraGM20BABI.h"),
        "git_sha1": attr.string(mandatory = True, doc = "MESA_GIT_SHA1_OVERRIDE: the 10-hex commit the version string names"),
        "build_id": attr.string(doc = "-Dnvk-build-id (empty: the pipeline cache UUID hashes the dylib's LC_UUID)"),
        "expect_tree": attr.string(doc = "the patched tree's git id, checked (the .source of the dylib this reproduces)"),
        "expect_sha256": attr.string(doc = "the dylib's sha256, checked: the action fails on any other bytes"),
        "rust_src_prefix": attr.string(mandatory = True),
        "_build": attr.label(default = ":build_nvk", executable = True, cfg = "exec"),
        "_relink": attr.label(default = ":relink", executable = True, cfg = "exec"),
        "_bindgen": attr.label(default = ":bindgen", executable = True, cfg = "exec"),
        "_cbindgen": attr.label(default = ":cbindgen", executable = True, cfg = "exec"),
        "_mesa": attr.label(default = "@mesa//:mesa.tar", allow_single_file = True),
        "_wraps": attr.label(default = "@mesa_wraps//:all"),
        "_wraps_marker": attr.label(default = "@mesa_wraps//:packagecache/syn-2.0.87.tar.gz", allow_single_file = True),
        "_ninja": attr.label(default = "@nvk_ninja//:ninja", allow_single_file = True),
        "_rust": attr.label(default = "@nvk_rust//:all"),
        "_rustc": attr.label(default = "@nvk_rust//:sysroot/bin/rustc", allow_single_file = True),
        "_libclang_files": attr.label(default = "@nvk_libclang//:all"),
        "_libclang": attr.label(default = "@nvk_libclang//:lib/libclang.dylib", allow_single_file = True),
        "_pinned": attr.label(default = "@pinned//:all"),
        "_pinned_inputs": attr.label(default = "@pinned//:nvk/inputs.txt", allow_single_file = True),
        "_python_deps": attr.label_list(default = [
            "@ioswitch_pypi//meson",
            "@ioswitch_pypi//mako",
            "@ioswitch_pypi//markupsafe",
            "@ioswitch_pypi//packaging",
            "@ioswitch_pypi//pyyaml",
        ]),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)
