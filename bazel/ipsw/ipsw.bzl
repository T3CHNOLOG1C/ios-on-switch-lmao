"""The IPSW's members, fetched by HTTP range request (no full IPSW download).

Each member is its own repository (@ipsw_<role>), so building something that needs the 16 MB kernelcache never
fetches the 5 GB OS image. Bazel downloads exactly the member's stored bytes with a `Range` header and checks them
against the pinned sha256, which puts them in the repository cache (content-addressed, shared by every output base).

A stored member (the OS dmg) is used as fetched. A deflated one is inflated by a build action (ipsw_inflate,
//bazel/ipsw:inflate) that checks the content against its pin; only the deflated bytes live in the repository cache.

@ipsw is a hub of aliases, one per role: @ipsw//:kernelcache, @ipsw//:os, ... (to //bazel/ipsw:<role> for deflated
members, the inflated file).
"""

load(":pins.bzl", "IPSW")

_STORED_BUILD = """\
exports_files([{name}], visibility = ["//visibility:public"])
"""

def _ipsw_member_impl(rctx):
    first, last = rctx.attr.byte_range.split("-")
    rctx.report_progress("{} (bytes {})".format(rctx.attr.member, rctx.attr.byte_range))
    rctx.download(
        url = rctx.attr.url,
        output = rctx.attr.output,
        sha256 = rctx.attr.sha256,
        headers = {"Range": ["bytes={}-{}".format(first, last)]},
    )
    rctx.file("BUILD.bazel", _STORED_BUILD.format(name = repr(rctx.attr.output)))

ipsw_member = repository_rule(
    implementation = _ipsw_member_impl,
    attrs = {
        "url": attr.string(mandatory = True),
        "member": attr.string(mandatory = True),
        "byte_range": attr.string(mandatory = True, doc = "first-last byte of the stored member in the IPSW"),
        "output": attr.string(mandatory = True, doc = "the member's file name, or stored.deflate"),
        "sha256": attr.string(mandatory = True, doc = "of the stored bytes"),
    },
)

def _ipsw_hub_impl(rctx):
    lines = ['package(default_visibility = ["//visibility:public"])', ""]
    for role, actual in sorted(rctx.attr.members.items()):
        lines.append('alias(name = "{}", actual = "{}")'.format(role, actual))
    lines.append("")
    lines.append("# {} {} ({}) for {}".format(rctx.attr.version, rctx.attr.build, rctx.attr.url, rctx.attr.product))
    rctx.file("BUILD.bazel", "\n".join(lines) + "\n")

_ipsw_hub = repository_rule(
    implementation = _ipsw_hub_impl,
    attrs = {
        "members": attr.string_dict(doc = "role -> label of its file"),
        "url": attr.string(),
        "version": attr.string(),
        "build": attr.string(),
        "product": attr.string(),
    },
)

def _ipsw_extension_impl(module_ctx):
    members = {}
    for role, pin in IPSW["members"].items():
        filename = pin["member"].split("/")[-1]
        output = "stored.deflate" if pin["deflated"] else filename
        ipsw_member(
            name = "ipsw_" + role,
            url = IPSW["url"],
            member = pin["member"],
            byte_range = "{}-{}".format(pin["offset"], pin["offset"] + pin["stored_size"] - 1),
            output = output,
            sha256 = pin["sha256"],
        )
        members[role] = "@@//bazel/ipsw:" + role if pin["deflated"] else "@ipsw_{}//:{}".format(role, filename)
    _ipsw_hub(
        name = "ipsw",
        members = members,
        url = IPSW["url"],
        version = IPSW["version"],
        build = IPSW["build"],
        product = IPSW["product"],
    )
    return module_ctx.extension_metadata(reproducible = True)

ipsw = module_extension(implementation = _ipsw_extension_impl)

def _ipsw_inflate_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "/" + ctx.attr.filename)
    ctx.actions.run(
        executable = ctx.executable._inflate,
        arguments = [ctx.file.src.path, out.path, ctx.attr.content_sha256],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "IpswInflate",
        progress_message = "Inflating the IPSW's " + ctx.attr.filename,
        # A multi-GB output (the system cryptex) would evict everything else from the 10 GB disk cache.
        execution_requirements = {"no-cache": ""} if ctx.attr.large else {},
    )
    return [DefaultInfo(files = depset([out]))]

ipsw_inflate = rule(
    implementation = _ipsw_inflate_impl,
    doc = "A deflated IPSW member's content (<name>/<filename>), checked against its content sha256.",
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True, doc = "the member's stored (deflated) bytes"),
        "filename": attr.string(mandatory = True),
        "content_sha256": attr.string(mandatory = True),
        "large": attr.bool(doc = "kept out of the disk and remote caches"),
        "_inflate": attr.label(default = "//bazel/ipsw:inflate", executable = True, cfg = "exec"),
    },
)

def ipsw_members():
    """One ipsw_inflate per deflated member of bazel/ipsw/pins.bzl (in //bazel/ipsw)."""
    for role, pin in IPSW["members"].items():
        if pin["deflated"]:
            ipsw_inflate(
                name = role,
                src = "@ipsw_{}//:stored.deflate".format(role),
                filename = pin["member"].split("/")[-1],
                content_sha256 = pin["content_sha256"],
                large = pin["stored_size"] > (256 << 20),
                visibility = ["//visibility:public"],
            )
