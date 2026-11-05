"""The tools Mesa's NVK build runs that are not Mesa (the `mesa_tools` module extension in MODULE.bazel), each pinned by
sha256. Mesa itself is vendored (third_party/mesa), as are the crates of its meson wraps (third_party/mesa/subprojects).

@nvk_rust        Rust 1.96.1 (the rustc NVK has been built with, commit 31fca3adb): rustc, cargo, rustfmt, rust-std for
                 aarch64-apple-darwin and aarch64-apple-ios, rust-src, merged into one sysroot, and rules_rust toolchains
                 over it for //bazel/rust:toolchain_version = 1.96.1 (MODULE.bazel registers them).
@nvk_bindgen_src / @nvk_cbindgen_src  bindgen-cli 0.73.2 and cbindgen 0.29.4 from crates.io, with every crate of
                 their own Cargo.lock vendored (built by //third_party/mesa/bazel:bindgen and :cbindgen).
@nvk_libclang    libclang.dylib and clang's builtin headers from the LLVM 23.1.4 release (bindgen loads it; the archive
                 is the one @llvm already fetched, so the repository cache holds it once).
"""

load("//gpu:deps.bzl", "LLVM_RELEASE", "lock_packages")

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

CARGO_TOOLS = {
    "nvk_bindgen_src": ("bindgen-cli", "0.73.2", "5622e043625b6d1f32c84f53699690a44d1419e657879de26eb252699f5ebb7c"),
    "nvk_cbindgen_src": ("cbindgen", "0.29.4", "2ecb53484c9c167ba674026b656d8a27d7657a58e6066aa902bfb1a4aa00ae20"),
}

_ALL_FILES = """\
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all",
    srcs = glob(["**"], exclude = ["BUILD.bazel", "REPO.bazel", "WORKSPACE", "WORKSPACE.bazel"]),
)
"""

# Rust 1.96.1 as rules_rust toolchains, chosen where //bazel/rust:toolchain_version is 1.96.1 (NVK's Rust, through
# third_party/mesa/bazel/defs.bzl's mesa_platform). Built here rather than by rules_rust's own repository_set because
# the standard library's sources (rust-src) must be in the sysroot rustc is given: with them rustc names the std code
# it instantiates in NAK by its real path (lib/rustlib/src/rust/library/...), which --remap-path-prefix spells as the
# rustup path main's dylib carries; without them those panic locations read /rustc/<commit>/library/....
_RUST_TOOLCHAINS = """
load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup", "rust_toolchain")

exports_files(["bin/rustc", "bin/cargo", "bin/rustfmt", "bin/rustdoc"])

# rustfmt and the compiler libraries it loads (@rpath ../lib).
filegroup(
    name = "rustfmt",
    srcs = ["bin/rustfmt"] + glob(["lib/*.dylib"]),
)

filegroup(
    name = "rustc_lib",
    srcs = glob([
        "lib/*.dylib*",
        "lib/rustlib/aarch64-apple-darwin/codegen-backends/*.dylib",
        "lib/rustlib/aarch64-apple-darwin/lib/*.dylib*",
        "lib/rustlib/aarch64-apple-darwin/lib/*.rmeta",
    ], allow_empty = True),
)

[rust_stdlib_filegroup(
    name = "rust_std-" + triple,
    srcs = glob([
        "lib/rustlib/{}/lib/*.rlib".format(triple),
        "lib/rustlib/{}/lib/*.rmeta".format(triple),
        "lib/rustlib/{}/lib/*.dylib*".format(triple),
        "lib/rustlib/{}/lib/*.a".format(triple),
        "lib/rustlib/{}/lib/self-contained/**".format(triple),
        "lib/rustlib/src/rust/**",
    ], allow_empty = True),
) for triple in ("aarch64-apple-darwin", "aarch64-apple-ios")]

[rust_toolchain(
    name = "rust_toolchain-" + triple,
    binary_ext = "",
    cargo = "bin/cargo",
    default_edition = "2021",
    dylib_ext = ".dylib",
    exec_triple = "aarch64-apple-darwin",
    rust_doc = "bin/rustdoc",
    rust_std = ":rust_std-" + triple,
    rustc = "bin/rustc",
    rustc_lib = ":rustc_lib",
    rustfmt = "bin/rustfmt",
    staticlib_ext = ".a",
    stdlib_linkflags = ["-lSystem", "-lresolv"],
    target_triple = triple,
    version = "1.96.1",
) for triple in ("aarch64-apple-darwin", "aarch64-apple-ios")]

[toolchain(
    name = "rust-" + triple,
    exec_compatible_with = ["@platforms//cpu:aarch64", "@platforms//os:osx"],
    target_compatible_with = ["@platforms//cpu:aarch64", os],
    target_settings = ["@@//bazel/rust:toolchain_1.96.1"],
    toolchain = ":rust_toolchain-" + triple,
    toolchain_type = "@rules_rust//rust:toolchain",
) for triple, os in (("aarch64-apple-darwin", "@platforms//os:osx"), ("aarch64-apple-ios", "@platforms//os:ios"))]
"""

def _rust_sysroot_impl(rctx):
    for archive, component, sha256 in RUST_COMPONENTS:
        rctx.download_and_extract(
            url = _RUST_DIST + archive + ".tar.xz",
            sha256 = sha256,
            stripPrefix = archive + "/" + component,
        )
    rctx.file("BUILD.bazel", _ALL_FILES + _RUST_TOOLCHAINS)

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

nvk_rust_sysroot = repository_rule(implementation = _rust_sysroot_impl)

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
        "sha256": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
    },
)

def _libclang_impl(rctx):
    archive = "llvm.tar.xz"
    rctx.download(url = LLVM_RELEASE["url"], output = archive, sha256 = LLVM_RELEASE["sha256"])
    prefix = LLVM_RELEASE["prefix"]
    result = rctx.execute([
        "/usr/bin/tar",
        "-xJf",
        archive,
        "--strip-components",
        "1",
        prefix + "/lib/libclang.dylib",
        prefix + "/lib/clang/23/include",
    ])
    if result.return_code:
        fail("extracting libclang: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", _ALL_FILES)

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

nvk_libclang = repository_rule(implementation = _libclang_impl)

def _mesa_tools_impl(module_ctx):
    nvk_rust_sysroot(name = "nvk_rust")
    for name, (crate, version, sha256) in CARGO_TOOLS.items():
        cargo_tool_source(name = name, crate = crate, version = version, sha256 = sha256)
    nvk_libclang(name = "nvk_libclang")
    return module_ctx.extension_metadata(reproducible = True)

mesa_tools = module_extension(implementation = _mesa_tools_impl)
