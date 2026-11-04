"""Rules behind //install: the kernel-derived install spec, the IPSW pins as JSON, the install artifacts."""

load("//bazel/ipsw:pins.bzl", "IPSW")

def _ipsw_pins_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(IPSW, indent = "  ") + "\n")
    return [DefaultInfo(files = depset([out]))]

ipsw_pins = rule(
    implementation = _ipsw_pins_impl,
    doc = "bazel/ipsw/pins.bzl as JSON (the installer checks Preboot copies against the content sha256s).",
)

def _install_spec_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    sources = {
        "raw-mount": ctx.file.raw_mount,
        "cryptex-h": ctx.file.cryptex_h,
        "cryptex-cpp": ctx.file.cryptex_cpp,
        "block-cpp": ctx.file.block_cpp,
    }
    args = ctx.actions.args()
    for flag, f in sources.items():
        args.add("--" + flag, f)
    args.add("--out", out)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = sources.values(),
        outputs = [out],
        mnemonic = "InstallSpec",
        progress_message = "Reading the SD layout the kernel expects from its sources",
    )
    return [DefaultInfo(files = depset([out]))]

install_spec = rule(
    implementation = _install_spec_impl,
    attrs = {
        "raw_mount": attr.label(allow_single_file = True, mandatory = True),
        "cryptex_h": attr.label(allow_single_file = True, mandatory = True),
        "cryptex_cpp": attr.label(allow_single_file = True, mandatory = True),
        "block_cpp": attr.label(allow_single_file = True, mandatory = True),
        "_tool": attr.label(default = "//tools/install:spec", executable = True, cfg = "exec"),
    },
    doc = "The partition size and volume/cryptex UUIDs the kernel was built with, parsed from its sources.",
)

def _install_artifacts_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    package = ctx.file.boot_package
    args = ctx.actions.args()
    args.add("--spec", ctx.file.spec)
    args.add("--ipsw", ctx.file.ipsw)
    args.add("--boot-package", package.path)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.spec, ctx.file.ipsw, package],
        outputs = [out],
        mnemonic = "InstallArtifacts",
        progress_message = "Describing the SD install",
    )
    return [DefaultInfo(files = depset([out]))]

install_artifacts = rule(
    implementation = _install_artifacts_impl,
    attrs = {
        "spec": attr.label(allow_single_file = True, mandatory = True),
        "ipsw": attr.label(allow_single_file = True, mandatory = True),
        "boot_package": attr.label(allow_single_file = True, mandatory = True),
        "_tool": attr.label(default = "//tools/install:artifacts", executable = True, cfg = "exec"),
    },
)

def _test_member_impl(ctx):
    member = ctx.actions.declare_file(ctx.label.name + ".bin")
    pins = ctx.actions.declare_file(ctx.label.name + ".pins.json")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [ctx.file.pins.path, ctx.attr.role, str(ctx.attr.mib), member.path, pins.path],
        inputs = [ctx.file.pins],
        outputs = [member, pins],
        mnemonic = "TestMember",
    )
    return [
        DefaultInfo(files = depset([member])),
        OutputGroupInfo(pins = depset([pins])),
    ]

test_member = rule(
    implementation = _test_member_impl,
    attrs = {
        "pins": attr.label(allow_single_file = True, mandatory = True),
        "role": attr.string(mandatory = True),
        "mib": attr.int(default = 4),
        "_tool": attr.label(default = "//tools/install:test_member", executable = True, cfg = "exec"),
    },
    doc = "A small stand-in for a large IPSW member plus pins that match it (low-space installer tests).",
)

def _fake_boot_package_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [out.path, "--generation", str(ctx.attr.generation)] + (["--gpu"] if ctx.attr.gpu else []),
        outputs = [out],
        mnemonic = "FakeBootPackage",
    )
    return [DefaultInfo(files = depset([out]))]

fake_boot_package = rule(
    implementation = _fake_boot_package_impl,
    attrs = {
        "generation": attr.int(default = 0),
        "gpu": attr.bool(default = False, doc = "make a fake //gpu:bundle instead"),
        "_tool": attr.label(default = "//tools/install:fake_boot_package", executable = True, cfg = "exec"),
    },
    doc = "A boot package with dummy contents and the real layout, for tests (--//deploy:boot_package=...).",
)
