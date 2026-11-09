"""@switch_release: the GitHub release assets of MODULE.bazel's module(version), which //:image builds from.

The version, not git, names the release: a Mac with only Bazelisk has no git, and the release's source zip has no
.git. The assets are checked against the release's SHA256SUMS. --repo_env=SWITCH_RELEASE_DIR=<dir> takes them and
SHA256SUMS from a local directory instead (CI, before the release is published).

The image inputs (kernel, loader, overlay, GPU trust) are per IPSW kind: image-inputs-<kind>.tar, each the release
carries laid out under inputs-<kind>/, and the targets below select the kind --//ipsw:kind names.
"""

load("//boot:overlay.bzl", "OVERLAY_BINARIES")
load("//ipsw:pins.bzl", "IPSWS")

REPO = "T3CHNOLOG1C/ios-on-switch-lmao"
ASSETS = {
    "host_tools": "host-tools-darwin-arm64.tar",
    "sd": "sd.zip",
}

# An Intel Mac takes the universal apfs_tool, lz4 and arm64e instead; that asset has no shader/ (the shader toolset is
# arm64 only), so the shader seed cannot be built there yet.
ASSETS_X86_64 = dict(ASSETS, host_tools = "host-tools-darwin-x86_64.tar")

KINDS = [kind for kind in IPSWS if kind]

def _inputs_asset(kind):
    return "image-inputs-{}.tar".format(kind)

# The targets //install and //boot take, by name: a directory's files, or one file.
_GROUPS = {
    "sd": "sd/**",
    "host_tools": "host_tools/**",
    "shader_tools": "host_tools/shader/**",
}
_FILES = {
    "install_payload": "sd/bootloader/payloads/install-ios.bin",
    "apfs_tool": "host_tools/apfs_tool",
    "lz4": "host_tools/lz4",
    "arm64e": "host_tools/arm64e",
}
_KIND_FILES = {
    "gpu_base": "gpu/trust.json",
    "gpu_trustcache": "gpu/TegraGM20BMetal.trustcache",
    "kernel": "kernel/kernel.macho",
    "prelink_options": "kernel/prelink.json",
    "loader": "loader/loader.uboot.bin",
    "loader_constants": "loader/constants.json",
} | {"overlay/" + source: "overlay/" + source for source in OVERLAY_BINARIES.values()}

def _select(kinds, value):
    cases = ", ".join(['"@@//ipsw:{}": {}'.format(kind, value(kind)) for kind in kinds])
    return "select({{{}}}, no_match_error = {})".format(
        cases,
        repr("the release carries image inputs for {} only: build this iOS's with --//install:prebuilt=local".format(
            ", ".join(kinds),
        )),
    )

def _build(kinds):
    return "\n\n".join(
        ['package(default_visibility = ["//visibility:public"])'] +
        ["filegroup(name = {}, srcs = glob([{}]))".format(repr(name), repr(files)) for name, files in _GROUPS.items()] +
        ["filegroup(name = {}, srcs = [{}])".format(repr(name), repr(path)) for name, path in _FILES.items()] +
        ["filegroup(name = \"inputs\", srcs = {})".format(
            _select(kinds, lambda kind: "glob([{}])".format(repr("inputs-{}/**".format(kind)))),
        )] +
        ["filegroup(name = {}, srcs = {})".format(
            repr(name),
            _select(kinds, lambda kind, path = path: "[{}]".format(repr("inputs-{}/{}".format(kind, path)))),
        ) for name, path in _KIND_FILES.items()] +
        ['exports_files(glob(["sd/**", "inputs-*/**", "host_tools/**"]))'],
    ) + "\n"

def _sums(text):
    sums = {}
    for line in text.splitlines():
        fields = [f for f in line.replace("\t", " ").split(" ") if f]
        if len(fields) == 2:
            sums[fields[1].lstrip("*")] = fields[0]
    return sums

_MISSING = """load(":missing.bzl", "missing")

package(default_visibility = ["//visibility:public"])

[missing(name = name) for name in {names}]
""".format(names = repr(list(_GROUPS) + list(_FILES) + ["inputs"] + list(_KIND_FILES)))

_MISSING_RULE = """def _impl(ctx):
    fail({message})

missing = rule(implementation = _impl)
"""

def _release_impl(rctx):
    version = rctx.attr.version
    local = rctx.getenv("SWITCH_RELEASE_DIR")
    base = "file://" + local if local else "https://github.com/{}/releases/download/{}".format(REPO, version)
    if not version or not rctx.download(url = base + "/SHA256SUMS", output = "SHA256SUMS", allow_fail = True).success:
        message = "no release assets for version \"{}\" at {}: build them locally (--//install:prebuilt=local)".format(
            version,
            base,
        )
        rctx.file("missing.bzl", _MISSING_RULE.format(message = repr(message)))
        rctx.file("BUILD.bazel", _MISSING)
        return
    sums = _sums(rctx.read("SHA256SUMS"))
    assets = dict(ASSETS_X86_64 if rctx.os.arch in ("amd64", "x86_64") else ASSETS)
    kinds = [kind for kind in KINDS if "ios-on-switch-lmao-{}-{}".format(version, _inputs_asset(kind)) in sums]
    if not kinds:
        fail("release {}: SHA256SUMS lists no image inputs ({})".format(version, ", ".join([_inputs_asset(k) for k in KINDS])))
    assets |= {"inputs-" + kind: _inputs_asset(kind) for kind in kinds}
    for directory, suffix in assets.items():
        name = "ios-on-switch-lmao-{}-{}".format(version, suffix)
        if name not in sums:
            fail("release {}: SHA256SUMS lists no {}".format(version, name))
        rctx.download_and_extract(url = base + "/" + name, sha256 = sums[name], output = directory, type = suffix.rpartition(".")[2])
    rctx.file("BUILD.bazel", _build(kinds))

switch_release = repository_rule(
    implementation = _release_impl,
    attrs = {"version": attr.string()},
)

def _release_extension_impl(mctx):
    version = ""
    for module in mctx.modules:
        if module.is_root:
            version = module.version
    switch_release(name = "switch_release", version = version)

release = module_extension(implementation = _release_extension_impl)
