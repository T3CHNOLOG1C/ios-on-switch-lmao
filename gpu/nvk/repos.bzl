"""NVK's external inputs (the `nvk` module extension in MODULE.bazel), each pinned by sha256 or commit.

@mesa            Mesa at MESA_COMMIT, unpatched, as mesa.tar (`git archive` of the commit: modes and symlinks kept, so
                 the build can name the patched tree by its git tree id), fetched by commit with the host's git.
                 The build action applies the patch series (gpu/nvk/BUILD.bazel).
@mesa_wraps      The meson wrap packages the NVK build uses (Rust crates NAK needs), from the wraps' own sha256s,
                 laid out as subprojects/packagecache.
@nvk_rust        Rust 1.96.1 (the rustc r9 was built with, commit 31fca3adb): rustc, cargo, rustfmt, rust-std for
                 aarch64-apple-darwin and aarch64-apple-ios, rust-src, merged into one sysroot.
@nvk_bindgen_src / @nvk_cbindgen_src  bindgen-cli 0.73.2 and cbindgen 0.29.4 from crates.io, with every crate of
                 their own Cargo.lock vendored (built by //gpu/nvk:bindgen and :cbindgen).
@nvk_libclang    libclang.dylib and clang's builtin headers from the LLVM 23.1.4 release (bindgen loads it; the archive
                 is the one @llvm already fetched, so the repository cache holds it once).
@nvk_ninja       ninja 1.13.2's macOS release binary.
"""

load("//bazel/gpu:deps.bzl", "LLVM_RELEASE", "lock_packages")

MESA_COMMIT = "77452b9092cc44d76d92180a12d3aaaf27a98382"

MESA_REMOTE = "https://gitlab.freedesktop.org/mesa/mesa.git"

RUST_VERSION = "1.96.1"

_RUST_DIST = "https://static.rust-lang.org/dist/2026-06-30/"

# (archive, component directory inside it, sha256): channel-rust-1.96.1.toml's xz_hash values.
RUST_COMPONENTS = [
    ("rustc-1.96.1-aarch64-apple-darwin", "rustc", "9b548f0665f85f3c7fd45165611e3dea79f048c69d163be193986310d204fc2c"),
    ("cargo-1.96.1-aarch64-apple-darwin", "cargo", "2f43d75e9ad3febae5022c6f295cf93b74131cfdb1293a83e291f878ea9585a0"),
    ("rust-std-1.96.1-aarch64-apple-darwin", "rust-std-aarch64-apple-darwin", "0d433a74c303febc915f8fa1091ef166445706461d0c96984ecb7303aa8208f5"),
    ("rust-std-1.96.1-aarch64-apple-ios", "rust-std-aarch64-apple-ios", "2e37eef9c15ed9d73c6967965183d119609bf621c2394f71e3344428a0cb4742"),
    ("rust-src-1.96.1", "rust-src", "b343b6553bc772225f6a2b5be5055017f29794dcf4e08020366b27a0a40fc1c2"),
    # Mesa's generators and bindgen format the Rust they write with it; the format reaches the binary through
    # panic locations (line numbers), so it is part of the toolchain.
    ("rustfmt-1.96.1-aarch64-apple-darwin", "rustfmt-preview", "ed0cc9d72c04e7c3c4b7a82ab7f1ce5e33132017d062d8f9be6adf6472e8f165"),
]

# subprojects/<name>.wrap: (source_filename, source_hash). The URL is crates.io's for the crate.
MESA_WRAPS = {
    "paste-1.0.14.tar.gz": "de3145af08024dea9fa9914f381a17b8fc6034dfb00f3a84013f7ff43f29ed4c",
    "proc-macro2-1.0.86.tar.gz": "5e719e8df665df0d1c8fbfd238015744736151d4445ec0836b8e628aae103b77",
    "quote-1.0.35.tar.gz": "291ec9ab5efd934aaf503a6466c5d5251535d108ee747472c3977cc5acc868ef",
    "rustc-hash-2.1.1.tar.gz": "357703d41365b4b27c590e3ed91eabb1b663f07c4c084095e60cbed4362dff0d",
    "syn-2.0.87.tar.gz": "25aa4ce346d03a6dcd68dd8b4010bcb74e54e62c90c573f394c46eae99aba32d",
    "unicode-ident-1.0.12.tar.gz": "3354b9ac3fae1ff6755cb6db53683adb661634f67557942dea4facebec0fee4b",
}

CARGO_TOOLS = {
    "nvk_bindgen_src": ("bindgen-cli", "0.73.2", "5622e043625b6d1f32c84f53699690a44d1419e657879de26eb252699f5ebb7c"),
    "nvk_cbindgen_src": ("cbindgen", "0.29.4", "2ecb53484c9c167ba674026b656d8a27d7657a58e6066aa902bfb1a4aa00ae20"),
}

NINJA = ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip",
         "c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b")

_ALL_FILES = """\
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all",
    srcs = glob(["**"], exclude = ["BUILD.bazel", "REPO.bazel", "WORKSPACE", "WORKSPACE.bazel"]),
)
"""

def _git(rctx, *args):
    result = rctx.execute(["/usr/bin/git"] + list(args), timeout = 1800)
    if result.return_code:
        fail("git {}: {}".format(" ".join(args), result.stderr))
    return result.stdout.strip()

def _mesa_source_impl(rctx):
    rctx.report_progress("fetching mesa " + MESA_COMMIT[:10] + " (one commit, depth 1)")
    _git(rctx, "init", "-q", "repo")
    _git(rctx, "-C", "repo", "fetch", "-q", "--depth=1", MESA_REMOTE, MESA_COMMIT)
    fetched = _git(rctx, "-C", "repo", "rev-parse", "FETCH_HEAD")
    if fetched != MESA_COMMIT:
        fail("fetched mesa {}, wanted {}".format(fetched, MESA_COMMIT))
    tree = _git(rctx, "-C", "repo", "rev-parse", "FETCH_HEAD^{tree}")
    _git(rctx, "-C", "repo", "archive", "--format=tar", "-o", "../mesa.tar", "FETCH_HEAD")
    rctx.delete("repo")
    rctx.file("tree", tree + "\n")
    rctx.file("BUILD.bazel", """\
# Mesa {commit} (tree {tree}) as `git archive` made it.
exports_files(["mesa.tar", "tree"], visibility = ["//visibility:public"])
""".format(commit = MESA_COMMIT, tree = tree))

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

mesa_source = repository_rule(implementation = _mesa_source_impl)

def _rust_sysroot_impl(rctx):
    for archive, component, sha256 in RUST_COMPONENTS:
        rctx.download_and_extract(
            url = _RUST_DIST + archive + ".tar.xz",
            sha256 = sha256,
            stripPrefix = archive + "/" + component,
            output = "sysroot",
        )
    rctx.file("BUILD.bazel", _ALL_FILES + """
exports_files(["sysroot/bin/rustc", "sysroot/bin/cargo"])
""")

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

nvk_rust_sysroot = repository_rule(implementation = _rust_sysroot_impl)

def _mesa_wraps_impl(rctx):
    for filename, sha256 in MESA_WRAPS.items():
        name, _, version = filename[:-len(".tar.gz")].rpartition("-")
        rctx.download(
            url = "https://static.crates.io/crates/{}/{}/download".format(name, version),
            output = "packagecache/" + filename,
            sha256 = sha256,
        )
    rctx.file("BUILD.bazel", _ALL_FILES)

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

mesa_wraps = repository_rule(implementation = _mesa_wraps_impl)

def _cargo_tool_source_impl(rctx):
    crate, version, sha256 = rctx.attr.crate, rctx.attr.version, rctx.attr.sha256
    rctx.download_and_extract(
        url = "https://static.crates.io/crates/{}/{}-{}.crate".format(crate, crate, version),
        sha256 = sha256,
        stripPrefix = "{}-{}".format(crate, version),
        output = "src",
        type = "tar.gz",
    )
    count = 0
    for package in lock_packages(rctx.read("src/Cargo.lock")):
        if package.get("source", "") != "registry+https://github.com/rust-lang/crates.io-index":
            continue
        name, version, checksum = package["name"], package["version"], package["checksum"]
        directory = "vendor/{}-{}".format(name, version)
        rctx.download_and_extract(
            url = "https://static.crates.io/crates/{}/{}-{}.crate".format(name, name, version),
            sha256 = checksum,
            stripPrefix = "{}-{}".format(name, version),
            output = directory,
            type = "tar.gz",
        )
        rctx.file(directory + "/.cargo-checksum.json", json.encode({"files": {}, "package": checksum}))
        count += 1
    rctx.file("BUILD.bazel", _ALL_FILES + """
# {crate} {version} and the {count} crates of its Cargo.lock.
exports_files(["src/Cargo.toml"])
""".format(crate = crate, version = version, count = count))

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

cargo_tool_source = repository_rule(
    implementation = _cargo_tool_source_impl,
    attrs = {
        "crate": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
        "sha256": attr.string(mandatory = True),
    },
)

def _libclang_impl(rctx):
    archive = "llvm.tar.xz"
    rctx.download(url = LLVM_RELEASE["url"], output = archive, sha256 = LLVM_RELEASE["sha256"])
    prefix = LLVM_RELEASE["prefix"]
    result = rctx.execute(["/usr/bin/tar", "-xJf", archive, "--strip-components", "1",
                           prefix + "/lib/libclang.dylib", prefix + "/lib/clang/23/include"])
    if result.return_code:
        fail("extracting libclang: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", _ALL_FILES)

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

nvk_libclang = repository_rule(implementation = _libclang_impl)

def _nvk_impl(module_ctx):
    mesa_source(name = "mesa")
    nvk_rust_sysroot(name = "nvk_rust")
    mesa_wraps(name = "mesa_wraps")
    for name, (crate, version, sha256) in CARGO_TOOLS.items():
        cargo_tool_source(name = name, crate = crate, version = version, sha256 = sha256)
    nvk_libclang(name = "nvk_libclang")
    return module_ctx.extension_metadata(reproducible = True)

nvk = module_extension(implementation = _nvk_impl)
