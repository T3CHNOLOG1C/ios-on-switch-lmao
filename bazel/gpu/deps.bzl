"""The GPU bundle's external inputs (MODULE.bazel `gpu` extension).

@m2v           steelbrain/metal2vulkan at 720ef1ce (GitHub tarball, sha256), the patch series of
               drivers/TegraGM20BMetal/patches/series applied in order. The validation crate (1.5 GB of corpus
               and fixtures the bridge never reads) is dropped but for its manifest and the two LLVM-text
               fixtures the bridge assembles. The crate layout (crates/<name>/ under the workspace Cargo.toml)
               is kept as is: metal2vulkan's and metal2vulkan-runtime's build scripts hash the sources by
               relative path into the shader cache's build id, which must equal a plain cargo build's.
@m2v_crates    every crates.io package of the bridge's Cargo.lock (drivers/TegraGM20BMetal/bridge/Cargo.lock)
               as a cargo `vendor` directory: each .crate fetched by the lock's own sha256.
@gpu_apple_libs  the Apple Metal libraries the seed and the raw pack read (QuartzCore's default.metallib among them),
               extracted from 21H461's OS volume (@ipsw_os, the IPSW's 044-76040-038.dmg) and pinned file by file
               in bazel/gpu/apple-libs.sha256. Each file is a repository cache entry of its own, so a fetch reads the
               OS image only for a file the cache lacks: once per machine.
@gpu_device_data  what only the Switch makes (precompile/harvest.py --pack): the driver records m2v wrote and the Metal
               libraries Core Image compiled there, one tar pinned by sha256 (DEVICE_DATA), fetched into the repository
               cache from out/gpu-device-data/ of the checkout once.
@gpu_llvm_as   bin/llvm-as of the LLVM 23.1.4 release.
"""

load("//bazel/host:files.bzl", "sha256_lines")
load("//bazel/ipsw:pins.bzl", "IPSW")

M2V = {
    "commit": "720ef1ce9ae2a2068999a8675717418b9416283e",
    "url": "https://github.com/steelbrain/metal2vulkan/archive/720ef1ce9ae2a2068999a8675717418b9416283e.tar.gz",
    "sha256": "6232a3df14ba9a666742fe6fde8cf8b4967eb2d438e6b7244659b23cb0fd80a1",
    "strip_prefix": "metal2vulkan-720ef1ce9ae2a2068999a8675717418b9416283e",
}

# @gpu_device_data: the tar harvest.py --pack wrote (out/gpu-device-data/<file>), by its sha256. Its records are of the
# NVK identities the device booted (vendor, device, driver version, pipelineCacheUUID); the seed takes its own NVK's.
# None on a machine without the tar: the seed then has no device records and lacks the 2 S and 2 C lines of Core Image's
# device-compiled kernels, which the device compiles on first use.
DEVICE_DATA = {
    "file": "m2v-device-533beda1b80c203d.tar",
    "sha256": "533beda1b80c203d6f5f52576c0254304cc80e2fe26a0a750e51e7172d8acf7a",
}

_SERIES = "//out/native-a57/root-v1/tree/drivers/TegraGM20BMetal:patches/series"
_CARGO_LOCK = "//out/native-a57/root-v1/tree/drivers/TegraGM20BMetal:bridge/Cargo.lock"

def _drivers_label(path):
    """A series line (a path below tree/drivers/) as a label in its driver's package."""
    package, _, file = path.partition("/")
    return Label("//out/native-a57/root-v1/tree/drivers/{}:{}".format(package, file))

# --- @m2v --------------------------------------------------------------------------------------------------------

_M2V_BUILD = """\
package(default_visibility = ["//visibility:public"])

# Everything cargo reads to build the bridge's two m2v dependencies (and their path dependencies).
filegroup(
    name = "srcs",
    srcs = glob(["**"], exclude = ["BUILD.bazel", "REPO.bazel"]),
)

exports_files([
    "crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll",
    "crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll",
])
"""

_KEEP = [
    "crates/metal2vulkan-validation/Cargo.toml",
    "crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll",
    "crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll",
]

def _run(rctx, args):
    result = rctx.execute(args, quiet = True)
    if result.return_code:
        fail("{} failed ({}):\n{}{}".format(" ".join([str(a) for a in args]), result.return_code, result.stdout, result.stderr))

def _m2v_source_impl(rctx):
    archive = "m2v.tar.gz"
    rctx.download(url = rctx.attr.url, output = archive, sha256 = rctx.attr.sha256)
    # The validation crate is 1.5 GB extracted; only _KEEP of it is read. macOS's bsdtar filters while extracting.
    prefix = rctx.attr.strip_prefix + "/"
    _run(rctx, ["/usr/bin/tar", "-xzf", archive, "--strip-components", "1",
                "--exclude", prefix + "crates/metal2vulkan-validation/*"])
    _run(rctx, ["/usr/bin/tar", "-xzf", archive, "--strip-components", "1"] + [prefix + path for path in _KEEP])
    rctx.delete(archive)
    # Exactly the series, in order. Bazel's own patcher misreads a format-patch's "---" separator line, so the
    # patches go through patch(1) with no fuzz: the result is byte-identical to the `git apply` of
    # build-tegra-gm20b-metal.py (checked against its checkout when this was written).
    for line in rctx.read(rctx.attr.series).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        patch = rctx.path(_drivers_label(line))
        rctx.watch(patch)
        _run(rctx, ["/usr/bin/patch", "-p1", "-F0", "-N", "-s", "--no-backup-if-mismatch", "-i", str(patch)])
    rctx.file("BUILD.bazel", _M2V_BUILD)
    # A pinned download and the patches it watches: the repository contents cache shares this across output bases and
    # worktrees.
    return rctx.repo_metadata(reproducible = True)

m2v_source = repository_rule(
    implementation = _m2v_source_impl,
    attrs = {
        "url": attr.string(mandatory = True),
        "sha256": attr.string(mandatory = True),
        "strip_prefix": attr.string(mandatory = True),
        "series": attr.label(mandatory = True, allow_single_file = True),
        # Every patch of the series, so that editing one refetches (the series is read for the order).
        "patches": attr.label_list(allow_files = True),
    },
)

# --- @m2v_crates ---------------------------------------------------------------------------------------------------

def lock_packages(text):
    """The [[package]] tables of a Cargo.lock as dicts of their string keys."""
    packages, current = [], None
    for line in text.splitlines():
        line = line.strip()
        if line == "[[package]]":
            current = {}
            packages.append(current)
        elif line.startswith("[") and line != "[[package]]":
            current = None
        elif current != None and " = \"" in line:
            key, _, value = line.partition(" = ")
            current[key] = value.strip('"')
    return packages

def _cargo_vendor_impl(rctx):
    names = []
    for package in lock_packages(rctx.read(rctx.attr.lockfile)):
        if package.get("source", "") != "registry+https://github.com/rust-lang/crates.io-index":
            continue
        name, version, checksum = package["name"], package["version"], package["checksum"]
        directory = "{}-{}".format(name, version)
        rctx.download_and_extract(
            url = "https://static.crates.io/crates/{}/{}-{}.crate".format(name, name, version),
            sha256 = checksum,
            stripPrefix = directory,
            output = directory,
            type = "tar.gz",
        )
        # A cargo `directory` source: the package checksum must equal the lock's; no per-file checksums.
        rctx.file(directory + "/.cargo-checksum.json", json.encode({"files": {}, "package": checksum}))
        names.append(directory)
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

# {count} crates of {lock}, laid out like `cargo vendor`.
filegroup(
    name = "vendor",
    srcs = glob(["**"], exclude = ["BUILD.bazel", "REPO.bazel"]),
)
""".format(count = len(names), lock = rctx.attr.lockfile))
    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

cargo_vendor = repository_rule(
    implementation = _cargo_vendor_impl,
    attrs = {"lockfile": attr.label(mandatory = True, allow_single_file = True)},
)

# --- @gpu_llvm_as --------------------------------------------------------------------------------------------------
# llvm-as of LLVM 23.1.4, the version the bridge's AIR fixtures were assembled with (Homebrew's; the bitcode records
# the writer's version, and the fixtures are compiled into the plugin). Only bin/llvm-as of the release archive is
# extracted; the archive is the same download (and sha256) as toolchains_llvm's, so the repository cache holds it once.
LLVM_RELEASE = {
    "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-macOS-ARM64.tar.xz",
    "sha256": "5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef",
    "prefix": "LLVM-23.1.4-macOS-ARM64",
}

def _llvm_tools_impl(rctx):
    archive = "llvm.tar.xz"
    rctx.download(url = LLVM_RELEASE["url"], output = archive, sha256 = LLVM_RELEASE["sha256"])
    _run(rctx, ["/usr/bin/tar", "-xJf", archive, "--strip-components", "1"] +
                [LLVM_RELEASE["prefix"] + "/bin/" + tool for tool in rctx.attr.tools])
    rctx.delete(archive)
    rctx.file("BUILD.bazel", 'package(default_visibility = ["//visibility:public"])\n\nexports_files({})\n'.format(
        repr(["bin/" + tool for tool in rctx.attr.tools]),
    ))
    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

llvm_tools = repository_rule(
    implementation = _llvm_tools_impl,
    attrs = {"tools": attr.string_list(mandatory = True)},
)

# --- @gpu_apple_libs -----------------------------------------------------------------------------------------------
# Apple's binaries stay out of the repository: they are read from the IPSW the build already fetches (bazel/ipsw), and
# only their sha256 is checked in.

# A URL nothing serves: a download of it succeeds only from the repository cache, by its sha256.
_CACHE_ONLY = "file:///ioswitch/repository-cache-only/"

_HDIUTIL = "/usr/bin/hdiutil"

def _from_image(rctx, files):
    """`files` ([(sha256, path below the volume's root)]) copied to root/<path> from the OS image (or the repository
    cache), each through Bazel's sha256 check, which adds it to the cache."""
    image = rctx.path(rctx.attr.image)
    mount = rctx.path("_volume")
    rctx.report_progress("Reading {} Metal libraries from {}".format(len(files), image.basename))
    if mount.exists:
        # A fetch interrupted while the image was attached.
        rctx.execute([_HDIUTIL, "detach", "-force", str(mount)], quiet = True)
        rctx.delete(mount)
    _run(rctx, [_HDIUTIL, "attach", "-readonly", "-nobrowse", "-noverify", "-noautoopen", "-mountpoint", str(mount), str(image)])
    failed = [
        path
        for sha256, path in files
        if not rctx.download(
            url = "file://{}/{}".format(mount, path),
            output = "root/" + path,
            sha256 = sha256,
            allow_fail = True,
        ).success
    ]
    detached = rctx.execute([_HDIUTIL, "detach", str(mount)], quiet = True)
    if failed:
        fail("{} has no file of the pinned sha256 at {} ({})".format(image.basename, ", ".join(failed), rctx.attr.pins))
    if detached.return_code:
        fail("hdiutil detach {} failed:\n{}{}".format(mount, detached.stdout, detached.stderr))
    rctx.delete(mount)

def _apple_libraries_impl(rctx):
    pins = sha256_lines(rctx.read(rctx.attr.pins))
    for at, (sha256, path) in enumerate(pins):
        if not rctx.download(url = _CACHE_ONLY + sha256, output = "root/" + path, sha256 = sha256, allow_fail = True).success:
            # The rest from the image: a download from it is still a cache hit for a file the cache has.
            _from_image(rctx, pins[at:])
            break
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

# {count} Metal libraries of {image}, at their paths below the volume's root (root/<device path>).
filegroup(
    name = "libraries",
    srcs = {files},
)

alias(
    name = "quartzcore",
    actual = "root/System/Library/Frameworks/QuartzCore.framework/default.metallib",
)
""".format(count = len(pins), image = rctx.attr.image, files = repr(["root/" + path for _, path in pins])))
    # Not marked reproducible: every file is already its own repository cache entry (a fetch is 152 hard links), and a
    # contents-cache entry made by a fetch that read the image would be checked against the 5.2 GB image it recorded.

apple_libraries = repository_rule(
    implementation = _apple_libraries_impl,
    doc = "The files a `shasum -a 256` listing names (paths below the OS volume's root) from the repository cache, or " +
          "from the OS image (attached read-only with hdiutil) for those the cache lacks.",
    attrs = {
        "pins": attr.label(mandatory = True, allow_single_file = True),
        "image": attr.label(mandatory = True, allow_single_file = True, doc = "the IPSW's OS volume image (@ipsw_os)"),
    },
)

# --- @gpu_device_data ----------------------------------------------------------------------------------------------

def _device_data_impl(rctx):
    if DEVICE_DATA:
        # Only a repository cache miss reads the file: the first fetch on a machine, from the checkout harvest.py ran
        # in.
        rctx.download_and_extract(
            url = "file://{}/out/gpu-device-data/{}".format(rctx.workspace_root, DEVICE_DATA["file"]),
            sha256 = DEVICE_DATA["sha256"],
            type = "tar",
        )
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

# {file} (sha256 {sha256}): precompile/harvest.py --pack of the device's m2v caches.
filegroup(
    name = "records",
    srcs = glob(["records/**"], allow_empty = True),
)

filegroup(
    name = "libraries",
    srcs = glob(["data/**"], allow_empty = True),
)
""".format(**(DEVICE_DATA or {"file": "no device data", "sha256": "none"})))
    # A download pinned by sha256 (the URL only seeds the repository cache): the repository contents cache shares this
    # across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

device_data = repository_rule(implementation = _device_data_impl)

# --- The extension -------------------------------------------------------------------------------------------------

def _gpu_impl(module_ctx):
    series = module_ctx.read(Label(_SERIES))
    patches = [
        _drivers_label(line.strip())
        for line in series.splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    m2v_source(
        name = "m2v",
        url = M2V["url"],
        sha256 = M2V["sha256"],
        strip_prefix = M2V["strip_prefix"],
        series = Label(_SERIES),
        patches = patches,
    )
    cargo_vendor(name = "m2v_crates", lockfile = Label(_CARGO_LOCK))
    llvm_tools(name = "gpu_llvm_as", tools = ["llvm-as"])
    apple_libraries(
        name = "gpu_apple_libs",
        pins = Label("//bazel/gpu:apple-libs.sha256"),
        image = Label("@ipsw_os//:" + IPSW["members"]["os"]["member"]),
    )
    device_data(name = "gpu_device_data")
    return module_ctx.extension_metadata(reproducible = True)

gpu = module_extension(implementation = _gpu_impl)
