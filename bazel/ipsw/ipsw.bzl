"""The IPSW's members as external repositories, fetched by HTTP range request (no full IPSW download).

Each member is its own repository (@ipsw_<role>), so building something that needs the 16 MB kernelcache never
fetches the 5 GB OS image. @ipsw is a hub of aliases: @ipsw//:kernelcache, @ipsw//:os, ...

Bazel downloads exactly the member's stored bytes with a `Range` header and checks them against the pinned
sha256, which puts them in the repository cache (content-addressed, shared by every output base; with
--experimental_repository_cache_hardlinks a cache hit is a hardlink, not a copy). A stored member (the OS dmg)
is used as downloaded; a deflated one is inflated next to it and checked against its content pin, and the
compressed copy is deleted from the repository (it stays in the repository cache).
"""

load(":pins.bzl", "IPSW")

_FILE_BUILD = """\
exports_files([{name}])

filegroup(
    name = "file",
    srcs = [{name}],
    visibility = ["//visibility:public"],
)
"""

def _ipsw_member_impl(rctx):
    filename = rctx.attr.member.split("/")[-1]
    first, last = rctx.attr.byte_range.split("-")
    stored = "stored.deflate" if rctx.attr.deflated else filename
    rctx.report_progress("{} (bytes {})".format(rctx.attr.member, rctx.attr.byte_range))
    rctx.download(
        url = rctx.attr.url,
        output = stored,
        sha256 = rctx.attr.sha256,
        headers = {"Range": ["bytes={}-{}".format(first, last)]},
    )
    if rctx.attr.deflated:
        result = rctx.execute([
            rctx.path(rctx.attr._python),
            "-I",
            rctx.path(rctx.attr._inflate),
            stored,
            filename,
            rctx.attr.content_sha256,
        ], quiet = False)
        if result.return_code:
            fail("inflating {} failed: {}".format(rctx.attr.member, result.stderr))
        rctx.delete(stored)
    rctx.file("BUILD.bazel", _FILE_BUILD.format(name = repr(filename)))

ipsw_member = repository_rule(
    implementation = _ipsw_member_impl,
    attrs = {
        "url": attr.string(mandatory = True),
        "member": attr.string(mandatory = True),
        "byte_range": attr.string(mandatory = True, doc = "first-last byte of the stored member in the IPSW"),
        "deflated": attr.bool(mandatory = True),
        "sha256": attr.string(mandatory = True),
        "content_sha256": attr.string(mandatory = True),
        "_python": attr.label(default = "@python_3_13_host//:python"),
        "_inflate": attr.label(default = ":inflate.py"),
    },
)

def _ipsw_hub_impl(rctx):
    lines = ['package(default_visibility = ["//visibility:public"])', ""]
    for role, filename in sorted(rctx.attr.members.items()):
        lines.append('alias(name = "{}", actual = "@ipsw_{}//:{}")'.format(role, role, filename))
    lines.append("")
    lines.append("# {} {} ({}) for {}".format(rctx.attr.version, rctx.attr.build, rctx.attr.url, rctx.attr.product))
    rctx.file("BUILD.bazel", "\n".join(lines) + "\n")
    rctx.file("identity.bzl", "IPSW = " + repr({
        "version": rctx.attr.version,
        "build": rctx.attr.build,
        "product": rctx.attr.product,
        "url": rctx.attr.url,
    }) + "\n")

_ipsw_hub = repository_rule(
    implementation = _ipsw_hub_impl,
    attrs = {
        "members": attr.string_dict(),
        "url": attr.string(),
        "version": attr.string(),
        "build": attr.string(),
        "product": attr.string(),
    },
)

def _ipsw_extension_impl(module_ctx):
    members = {}
    for role, pin in IPSW["members"].items():
        ipsw_member(
            name = "ipsw_" + role,
            url = IPSW["url"],
            member = pin["member"],
            byte_range = "{}-{}".format(pin["offset"], pin["offset"] + pin["stored_size"] - 1),
            deflated = pin["deflated"],
            sha256 = pin["sha256"],
            content_sha256 = pin["content_sha256"],
        )
        members[role] = pin["member"].split("/")[-1]
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
