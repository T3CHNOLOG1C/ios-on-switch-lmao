"""The GPU bundle's external inputs (MODULE.bazel `gpu` extension).

@gpu_apple_libs  the Apple Metal libraries the seed and the raw pack read (QuartzCore's default.metallib among them),
               extracted from 21H461's OS volume (@ipsw_os, the IPSW's 044-76040-038.dmg) and pinned file by file
               in gpu/apple-libs.sha256. Each file is a repository cache entry of its own, so a fetch reads the
               OS image only for a file the cache lacks: once per machine.
@gpu_device_data  what only the Switch makes (precompile/harvest.py --pack): the driver records m2v wrote and the Metal
               libraries Core Image compiled there, one tar pinned by sha256 (DEVICE_DATA), fetched into the repository
               cache once from ~/Library/Application Support/ioswitch/gpu-device-data/ (or
               $IOSWITCH_HOME/gpu-device-data/), where harvest.py --pack writes it; never from a checkout.
@gpu_llvm_as   bin/llvm-as of the LLVM 23.1.4 release.
"""

load("//bazel/host:files.bzl", "sha256_lines")
load("//ipsw:ipsw.bzl", "ipsw_member_label")
load("//ipsw:pins.bzl", "DEFAULT_KIND")

# @gpu_device_data: the tar harvest.py --pack wrote (<state dir>/gpu-device-data/<file>), by its sha256. Its records are of the
# NVK identities the device booted (vendor, device, driver version, pipelineCacheUUID); the seed takes its own NVK's.
# None on a machine without the tar: the seed then has no device records and lacks the 2 S and 2 C lines of Core Image's
# device-compiled kernels, which the device compiles on first use.
DEVICE_DATA = {
    "file": "m2v-device-533beda1b80c203d.tar",
    "sha256": "533beda1b80c203d6f5f52576c0254304cc80e2fe26a0a750e51e7172d8acf7a",
}

def _run(rctx, args):
    result = rctx.execute(args, quiet = True)
    if result.return_code:
        fail("{} failed ({}):\n{}{}".format(" ".join([str(a) for a in args]), result.return_code, result.stdout, result.stderr))

# --- Cargo.lock reading (gpu/nvk/repos.bzl vendors bindgen's and cbindgen's crates with it) ----------------------------

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
# Apple's binaries stay out of the repository: they are read from the IPSW the build already fetches (//ipsw), and
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

def state_dir(rctx):
    """Where the tools keep what is not the repository's: ~/Library/Application Support/ioswitch, or $IOSWITCH_HOME."""
    return rctx.getenv("IOSWITCH_HOME") or rctx.getenv("HOME", "") + "/Library/Application Support/ioswitch"

def _device_data_impl(rctx):
    if DEVICE_DATA:
        # Only a repository cache miss reads the file (the first fetch on a machine): from the tools' state directory,
        # where harvest.py --pack writes it, never from a checkout. The device's data is Apple-derived (Core Image's
        # compiled kernels, NVK's records of Apple's shaders), so it stays out of the repository, like the IPSW.
        where = state_dir(rctx) + "/gpu-device-data/" + DEVICE_DATA["file"]
        fetched = rctx.download_and_extract(
            url = "file://" + where.replace(" ", "%20"),
            sha256 = DEVICE_DATA["sha256"],
            type = "tar",
            allow_fail = True,
        )
        if not fetched.success:
            fail(("@gpu_device_data: {} (sha256 {}) is in neither the repository cache nor {}: copy it there from a " +
                  "machine that has it, or remake it (drivers/TegraGM20BMetal/precompile/harvest.py --pack)").format(
                DEVICE_DATA["file"],
                DEVICE_DATA["sha256"],
                where,
            ))
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
    llvm_tools(name = "gpu_llvm_as", tools = ["llvm-as"])
    apple_libraries(
        name = "gpu_apple_libs",
        pins = Label("//gpu:apple-libs.sha256"),
        image = ipsw_member_label(DEFAULT_KIND, "os"),
    )
    device_data(name = "gpu_device_data")
    return module_ctx.extension_metadata(reproducible = True)

gpu = module_extension(implementation = _gpu_impl)
