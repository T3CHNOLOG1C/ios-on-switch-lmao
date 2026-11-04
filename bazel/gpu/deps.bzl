"""The GPU bundle's external inputs (MODULE.bazel `gpu` extension).

@m2v           steelbrain/metal2vulkan at 720ef1ce (GitHub tarball, sha256), the patch series of
               drivers/TegraGM20BMetal/patches/series applied in order. The validation crate (1.5 GB of corpus
               and fixtures the bridge never reads) is dropped but for its manifest and the two LLVM-text
               fixtures the bridge assembles. The crate layout (crates/<name>/ under the workspace Cargo.toml)
               is kept as is: metal2vulkan's and metal2vulkan-runtime's build scripts hash the sources by
               relative path into the shader cache's build id, which must equal a plain cargo build's.
@m2v_crates    every crates.io package of the bridge's Cargo.lock (drivers/TegraGM20BMetal/bridge/Cargo.lock)
               as a cargo `vendor` directory: each .crate fetched by the lock's own sha256.
@gpu_host_inputs  inputs that exist only on the build Mac, each checked against a pin here (like Xcode):
               the NVK dylib and its host twin (Mesa 77452b90 + gpu/nvk/*.patch, built by hand: TEMPORARY until
               NVK builds under Bazel), 21H461's QuartzCore default.metallib, and the precompile's library store
               (Apple metallibs harvested from the device). Found under $IOSWITCH_NVK_DIR / $IOSWITCH_GPU_ARTIFACTS,
               default <workspace>/out/gpu/nvk and <workspace>/out/gpu-artifacts.
@gpu_llvm_as   bin/llvm-as of the LLVM 23.1.4 release.

NVK is not built here yet. What a hermetic NVK needs, beyond what this file already pins (Rust 1.98, Xcode, LLVM
23.1.4's release archive): Mesa 77452b9092cc44d76d92180a12d3aaaf27a98382 (gitlab.freedesktop.org/mesa/mesa; a
git_repository by commit, GitLab's archives are not byte-stable) + gpu/nvk/{nvk-darwin, nvk-0002-cmd-mem-written-
range, nvk-linear-color-attachments, nvk-0003-linear-color-attachment-rule, nvk-0004-sandbox-safe-darwin, nvk-0005-join-copy-regions}.patch in
that order, then gpu/nvk/relink-nvk.py; meson + ninja + mako
+ pyyaml + packaging + markupsafe from pinned wheels; bindgen and cbindgen (cargo, vendored); Mesa's meson wraps
(crates paste, syn, quote, proc-macro2, unicode-ident, rustc-hash); and for the two build-time tools a cross build
runs (mesa_clc, vtn_bindgen2) a native Mesa build against LLVM + clang libraries (the release archive has them, ~10k
files, several GB extracted), libclc (not in the release: build llvm-project 23.1.4's libclc, which needs
llvm-spirv), SPIRV-LLVM-Translator 23.x and SPIRV-Tools/SPIRV-Headers (CMake builds), glslang, and Xcode's bison/flex.
The iOS build itself is drivers' -Dvulkan-drivers=nouveau ... -Dmesa-clc=system -Dprecomp-compiler=system cross build
(out/agent-scratch/r4-encode/build-nvk-ios.sh), plus a pinned -Dnvk-build-id and MESA_GIT_SHA1_OVERRIDE so the dylib
is reproducible and its pipeline cache UUID stable.
"""

load("//bazel/host:files.bzl", "copy_checked", "sha256_lines")

M2V = {
    "commit": "720ef1ce9ae2a2068999a8675717418b9416283e",
    "url": "https://github.com/steelbrain/metal2vulkan/archive/720ef1ce9ae2a2068999a8675717418b9416283e.tar.gz",
    "sha256": "6232a3df14ba9a666742fe6fde8cf8b4967eb2d438e6b7244659b23cb0fd80a1",
    "strip_prefix": "metal2vulkan-720ef1ce9ae2a2068999a8675717418b9416283e",
}

# Host inputs: file name in the repository -> (environment variable naming the directory, default below the
# workspace, file name there, sha256). Directories: (variable, default, name, per-file `shasum -a 256` listing).
HOST_FILES = {
    # NVK r9 (gpu/nvk/build-nvk.sh, into out/gpu/nvk): Mesa 77452b90 + nvk-darwin, nvk-0002, nvk-linear-color-attachments, nvk-0003,
    # nvk-0004-sandbox-safe-darwin, nvk-0005-join-copy-regions, then relink-nvk.py (absent entry points absolute at 0).
    "nvk/libvulkan_nouveau.dylib": ("IOSWITCH_NVK_DIR", "out/gpu/nvk", "libvulkan_nouveau.r9.dylib", "276260dc81da7c40edd8cf7b95f796d41cc4eb84344bddcf78bae8f1d2876a58"),
    "nvk/libvulkan_nouveau.dylib.source": ("IOSWITCH_NVK_DIR", "out/gpu/nvk", "libvulkan_nouveau.r9.dylib.source", "5e5a48d11e3b141e8b59697fef1c9a82bc50ebb4487b00d0f55a0b6cb64930c2"),
    "nvk/libvulkan_nouveau.host.dylib": ("IOSWITCH_NVK_DIR", "out/gpu/nvk", "libvulkan_nouveau.r9.host.dylib", "de205f8f0779ddc583f5e7f25c8d8235497d395c2e142e10fd7509c20c6d34da"),
    "nvk/libvulkan_nouveau.host.dylib.source": ("IOSWITCH_NVK_DIR", "out/gpu/nvk", "libvulkan_nouveau.r9.host.dylib.source", "5e5a48d11e3b141e8b59697fef1c9a82bc50ebb4487b00d0f55a0b6cb64930c2"),
    "QuartzCore-21H461.default.metallib": ("IOSWITCH_GPU_ARTIFACTS", "out/gpu-artifacts", "QuartzCore-21H461.default.metallib", "606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9"),
}
HOST_DIRS = {
    "m2v-libs": ("IOSWITCH_GPU_ARTIFACTS", "out/gpu-artifacts", "m2v-libs", "//bazel/gpu:m2v-libs.sha256"),
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

def _lock_packages(text):
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
    for package in _lock_packages(rctx.read(rctx.attr.lockfile)):
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

llvm_tools = repository_rule(
    implementation = _llvm_tools_impl,
    attrs = {"tools": attr.string_list(mandatory = True)},
)

# --- @gpu_host_inputs ----------------------------------------------------------------------------------------------

def _host_dir(rctx, variable, default):
    return rctx.getenv(variable) or (str(rctx.workspace_root) + "/" + default)

def _host_input(rctx, name, variable, default, source, sha256):
    path = rctx.path(_host_dir(rctx, variable, default) + "/" + source)
    if not path.exists:
        fail("GPU host input {} is missing: {} (set {} to its directory)".format(name, path, variable))
    copy_checked(rctx, path, name, sha256)

def _gpu_host_inputs_impl(rctx):
    files = []
    for name, (variable, default, source, sha256) in HOST_FILES.items():
        _host_input(rctx, name, variable, default, source, sha256)
        files.append(name)
    for name, (variable, default, source, listing) in HOST_DIRS.items():
        for sha256, relative in sha256_lines(rctx.read(Label(listing))):
            _host_input(rctx, name + "/" + relative, variable, default, source + "/" + relative, sha256)
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

exports_files({files} + ["m2v-libs/names.json"])

filegroup(
    name = "nvk",
    srcs = glob(["nvk/*"]),
)

filegroup(
    name = "m2v_libs",
    srcs = glob(["m2v-libs/**"]),
)
""".format(files = repr(sorted(files))))

# Each input is copied in through Bazel's own sha256 check (bazel/host/files.bzl copy_checked) and watched, so
# changing one refetches the repository and a wrong one fails the fetch.
gpu_host_inputs = repository_rule(
    implementation = _gpu_host_inputs_impl,
    environ = ["IOSWITCH_NVK_DIR", "IOSWITCH_GPU_ARTIFACTS"],
)

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
    gpu_host_inputs(name = "gpu_host_inputs")
    return module_ctx.extension_metadata(reproducible = False)

gpu = module_extension(implementation = _gpu_impl)
