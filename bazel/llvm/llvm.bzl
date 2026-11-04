"""@llvm: the LLVM 23.1.4 binaries the build runs, from the official release (the version Homebrew had when the CMake
flow was qualified). Only these members of the archive are extracted (the whole release is 7.6 GB unpacked); the
archive itself is the one download the repository cache keeps (the GPU's @gpu_llvm_as reads the same sha256)."""

LLVM_RELEASE = {
    "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-macOS-ARM64.tar.xz",
    "sha256": "5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef",
    "prefix": "LLVM-23.1.4-macOS-ARM64",
}

# ld.lld, the loader's linker (a symlink to lld); llvm-objcopy (late objects, loader.bin); llvm-readelf (a symlink to
# llvm-readobj) and llvm-objdump (the loader builder's checks); clang-format and clang-tidy (//lint).
TOOLS = ["lld", "ld.lld", "llvm-objcopy", "llvm-readobj", "llvm-readelf", "llvm-objdump", "clang-format", "clang-tidy"]

def _llvm_impl(rctx):
    archive = "llvm.tar.xz"
    rctx.download(url = LLVM_RELEASE["url"], output = archive, sha256 = LLVM_RELEASE["sha256"])
    result = rctx.execute(["/usr/bin/tar", "-xJf", archive, "--strip-components", "1"] +
                          [LLVM_RELEASE["prefix"] + "/bin/" + tool for tool in TOOLS])
    if result.return_code:
        fail("extracting LLVM tools failed: " + result.stderr)
    rctx.delete(archive)
    rctx.file("BUILD.bazel", 'package(default_visibility = ["//visibility:public"])\n\nexports_files({})\n'.format(
        repr(["bin/" + tool for tool in TOOLS]),
    ))

llvm_tools = repository_rule(implementation = _llvm_impl)
