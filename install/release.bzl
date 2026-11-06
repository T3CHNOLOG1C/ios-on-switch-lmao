"""@switch_release: the GitHub release assets of MODULE.bazel's module(version), which //:image builds from.

The version, not git, names the release: a Mac with only Bazelisk has no git, and the release's source zip has no
.git. The assets are checked against the release's SHA256SUMS. --repo_env=SWITCH_RELEASE_DIR=<dir> takes them and
SHA256SUMS from a local directory instead (CI, before the release is published).
"""

REPO = "brunostjohn/ios-on-switch-lmao"
ASSETS = {
    "host_tools": "host-tools-darwin-arm64.tar",
    "inputs": "image-inputs.tar",
    "sd": "sd.zip",
}

_BUILD = """package(default_visibility = ["//visibility:public"])

filegroup(name = "sd", srcs = glob(["sd/**"]))

filegroup(name = "inputs", srcs = glob(["inputs/**"]))

filegroup(name = "host_tools", srcs = glob(["host_tools/**"]))

filegroup(name = "shader_tools", srcs = glob(["host_tools/shader/**"]))

filegroup(name = "gpu_base", srcs = ["inputs/gpu/trust.json"])

filegroup(name = "install_payload", srcs = ["sd/bootloader/payloads/install-ios.bin"])

exports_files(glob(["sd/**", "inputs/**", "host_tools/**"]))
"""

def _sums(text):
    sums = {}
    for line in text.splitlines():
        fields = [f for f in line.replace("\t", " ").split(" ") if f]
        if len(fields) == 2:
            sums[fields[1].lstrip("*")] = fields[0]
    return sums

_MISSING = """package(default_visibility = ["//visibility:public"])

load(":missing.bzl", "missing")

[missing(name = name) for name in ["sd", "inputs", "host_tools", "shader_tools", "gpu_base", "install_payload"]]
"""

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
    for directory, suffix in ASSETS.items():
        name = "ios-on-switch-lmao-{}-{}".format(version, suffix)
        if name not in sums:
            fail("release {}: SHA256SUMS lists no {}".format(version, name))
        rctx.download_and_extract(url = base + "/" + name, sha256 = sums[name], output = directory, type = suffix.rpartition(".")[2])
    rctx.file("BUILD.bazel", _BUILD)

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
