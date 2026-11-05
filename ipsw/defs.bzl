"""What the build derives from an IPSW's members. Each rule takes the IPSW (//bazel/ipsw:current, the kind
--//bazel/ipsw:kind selects) for the kind's derived pins and names, and its members from @ipsw (the same kind's)."""

load("//bazel/ipsw:ipsw.bzl", "IpswInfo")

_IPSW = {"ipsw": attr.label(default = "//bazel/ipsw:current", providers = [IpswInfo])}

def _ipsw_kernelcache_impl(ctx):
    ipsw = ctx.attr.ipsw[IpswInfo]
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._im4p,
        arguments = [
            ctx.file.src.path,
            out.path,
            "--expect-type=krnl",
            "--lzfse",
            "--thin-arm64",
            "--sha256=" + ipsw.derived["kernelcache_sha256"],
        ],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswKernelcache",
        progress_message = "Decompressing the %s kernelcache" % ipsw.build,
        # LZFSE through macOS's compression_tool.
        execution_requirements = {"requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_kernelcache = rule(
    implementation = _ipsw_kernelcache_impl,
    doc = "The kernelcache's IM4P 'krnl' payload, LZFSE-decompressed, its arm64 slice; checked against the kind's pin.",
    attrs = _IPSW | {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.string(mandatory = True),
        "_im4p": attr.label(default = "//tools/kernel:im4p", executable = True, cfg = "exec"),
    },
)

def _ipsw_static_trust_impl(ctx):
    ipsw = ctx.attr.ipsw[IpswInfo]
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._trust_bundle,
        arguments = [out.path, ctx.file.static.path, ctx.file.system.path],
        inputs = [ctx.file.static, ctx.file.system],
        outputs = [out],
        mnemonic = "IpswStaticTrust",
        progress_message = "Bundling the %s root and OS cryptex trust caches" % ipsw.build,
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_static_trust = rule(
    implementation = _ipsw_static_trust_impl,
    doc = "The static trust cache bundle for the root and the OS cryptex: the StaticTrustCache's 'trst' payload and " +
          "the Cryptex1,SystemTrustCache's 'trcs' payload (u32 count 2, u32 offsets).",
    attrs = _IPSW | {
        "static": attr.label(mandatory = True, allow_single_file = True),
        "system": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.string(mandatory = True),
        "_trust_bundle": attr.label(default = "//tools/kernel:trust_bundle", executable = True, cfg = "exec"),
    },
)

def _ipsw_pins_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(ctx.attr.ipsw[IpswInfo].pins, indent = "  ") + "\n")
    return [DefaultInfo(files = depset([out]))]

ipsw_pins = rule(
    implementation = _ipsw_pins_impl,
    doc = "The kind's member pins as JSON (the SD installer checks Preboot copies against their content sha256s).",
    attrs = _IPSW,
)
