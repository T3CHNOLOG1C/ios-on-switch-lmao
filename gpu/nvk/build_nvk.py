"""NVK (Mesa's Vulkan driver for nouveau, with the darwin nvkmd backend) as libvulkan_nouveau.dylib, for iOS or as
its macOS host twin, the way out/gpu/nvk/build-nvk.sh built r9 by hand, inside one Bazel action (gpu/nvk/defs.bzl).

  build_nvk.py --platform ios|macos --mesa mesa.tar --patch P... --wraps DIR --abi-header H --python-path P...
               --ninja N --rust-sysroot DIR --bindgen B --cbindgen C --libclang DIR --pinned DIR --relink R
               --xcode-env F --git-sha1 S [--build-id I] [--expect-tree T] [--expect-sha256 D]
               --rust-src-prefix P --out DYLIB --out-source SOURCE

Steps: unpack Mesa (`git archive` of the pinned commit), apply the patch series in order, name the result by its git
tree id (what <dylib>.source records and //gpu:seed compares between the dylib and its twin), meson setup + configure
+ ninja in <work>/mesa and <work>/nvk-<platform>-build (the hand build's layout: Mesa's own paths stay `../mesa/...`),
then relink-nvk.py (absent entry points absolute at 0).

Inputs that are not built here, each named and checked:
- Xcode (clang, ld, the SDKs) through xcrun, as everywhere in this repository.
- mesa_clc, vtn_bindgen2 and glslangValidator are stand-ins (`build_nvk.py stand-in <tool>`) that write the pinned
  outputs in bazel/pinned/nvk after checking every input those were generated from (bazel/pinned/nvk/inputs.txt).
- Rust panic locations inside the standard library are spelled as on the Mac that built r9 (a rustup toolchain with
  rust-src): --rust-src-prefix is mapped over this sysroot, so the strings, and the dylib, come out the same.
"""
import argparse
import hashlib
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

MESON_OPTIONS = [
    "-Dvulkan-drivers=nouveau", "-Dgallium-drivers=", "-Dplatforms=", "-Dglx=disabled", "-Degl=disabled",
    "-Dgles1=disabled", "-Dgles2=disabled", "-Dopengl=false", "-Dgbm=disabled", "-Dllvm=disabled",
    "-Dmesa-clc=system", "-Dprecomp-compiler=system", "-Dbuildtype=release", "-Db_ndebug=true", "-Dvalgrind=disabled",
    "-Dlibunwind=disabled", "-Dzstd=disabled", "-Dzlib=disabled", "-Dexpat=disabled", "-Dxmlconfig=disabled",
    "-Dshader-cache=disabled",
]
TARGET = "src/nouveau/vulkan/libvulkan_nouveau.dylib"
IOS_TRIPLE = "arm64-apple-ios17.0"
GLSLANG_VERSION = "Glslang Version: 11:16.6.0\nESSL Version: OpenGL ES GLSL 3.20 glslang Khronos. 16.6.0\n"


# --- git tree ids ------------------------------------------------------------------------------------------------------

def git_object(kind, data):
    return hashlib.sha1(b"%s %d\0" % (kind, len(data)) + data).digest()


def git_blob(data):
    """A file's blob as `git add` stores it in Mesa (`* text=auto`): a text file's CRLF line ends become LF. Only the
    .csv files Mesa checks out with eol=crlf (and `git archive` exports so) have them."""
    if b"\r\n" in data and b"\0" not in data[:8000]:
        data = data.replace(b"\r\n", b"\n")
    return git_object(b"blob", data)


def git_tree(directory):
    """The git tree id of a directory as `git add -A && git write-tree` gives it (no .gitignore'd files here)."""
    entries = []
    for name in os.listdir(directory):
        path = os.path.join(directory, name)
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            entries.append((name, b"120000", git_object(b"blob", os.readlink(path).encode())))
        elif stat.S_ISDIR(st.st_mode):
            sub = git_tree(path)
            if sub is not None:
                entries.append((name + "/", b"40000", sub))
        else:
            mode = b"100755" if st.st_mode & stat.S_IXUSR else b"100644"
            entries.append((name, mode, git_blob(Path(path).read_bytes())))
    if not entries:
        return None
    entries.sort(key=lambda e: e[0].encode())
    body = b"".join(mode + b" " + name.rstrip("/").encode() + b"\0" + sha for name, mode, sha in entries)
    return git_object(b"tree", body)


# --- stand-ins for the CL and GLSL compilers ----------------------------------------------------------------------------

def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_inputs(tool):
    """Every input the pinned outputs were generated from, as pinned (cwd is the build directory)."""
    pinned = Path(os.environ["NVK_PINNED"])
    bad = []
    for line in (pinned / "inputs.txt").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        owner, digest, path = line.split(None, 2)
        if owner != tool:
            continue
        local = path[len("build/"):] if path.startswith("build/") else os.path.join("../mesa", path)
        if not os.path.exists(local) or sha256(local) != digest:
            bad.append(path)
    if bad:
        sys.exit("{}: the pinned outputs in bazel/pinned/nvk were generated from other inputs than {}; regenerate them "
                 "with the real tool and update bazel/pinned/nvk/inputs.txt".format(tool, bad))
    return pinned


def stand_in(tool, argv):
    if tool == "glslangValidator":
        if "--version" in argv:
            sys.stdout.write(GLSLANG_VERSION)
            return 0
        pinned = check_inputs("glslang")
        out, depfile = argv[argv.index("-o") + 1], argv[argv.index("--depfile") + 1]
        shutil.copyfile(pinned / "astc_spv.h", out)
        Path(depfile).write_text("{}: {}\n".format(out, argv[argv.index(out) + 1]))
        return 0
    if tool == "mesa_clc":
        pinned = check_inputs("mesa_clc")
        out, depfile = argv[argv.index("-o") + 1], argv[argv.index("--depfile") + 1]
        sources = [a for a in argv[:argv.index("--")] if a.endswith(".cl")]
        shutil.copyfile(pinned / "nvkcl.spv", out)
        Path(depfile).write_text("{}: {}\n".format(out, " ".join(sources)))
        return 0
    if tool == "vtn_bindgen2":
        spv, cpp, header = argv
        pinned = Path(os.environ["NVK_PINNED"])
        if sha256(spv) != sha256(pinned / "nvkcl.spv"):
            sys.exit("vtn_bindgen2: {} is not the pinned nvkcl.spv".format(spv))
        shutil.copyfile(pinned / "nvkcl.cpp", cpp)
        shutil.copyfile(pinned / "nvkcl.h", header)
        return 0
    sys.exit("no stand-in for " + tool)


# --- the build ---------------------------------------------------------------------------------------------------------

def run(cmd, **kwargs):
    result = subprocess.run(cmd, **kwargs)
    if result.returncode:
        sys.exit("FAILED ({}): {}".format(result.returncode, " ".join(shlex.quote(str(c)) for c in cmd)))


def write_script(path, text):
    path.write_text(text)
    path.chmod(0o755)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "stand-in":
        return stand_in(sys.argv[2], sys.argv[3:])
    p = argparse.ArgumentParser()
    p.add_argument("--platform", choices=["ios", "macos"], required=True)
    for name in ("mesa", "wraps", "abi-header", "ninja", "rust-sysroot", "bindgen", "cbindgen", "libclang", "pinned",
                 "relink", "xcode-env", "git-sha1", "rust-src-prefix", "out", "out-source"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--patch", action="append", default=[])
    p.add_argument("--python-path", action="append", default=[])
    p.add_argument("--build-id", default="")
    p.add_argument("--expect-tree")
    p.add_argument("--expect-sha256")
    args = p.parse_args()
    here = Path.cwd()
    xcode = dict(line.split("=", 1) for line in Path(args.xcode_env).read_text().splitlines() if "=" in line)
    # rustc names its sysroot by its own resolved location (the file, not the sandbox's link to it): map that one.
    sysroot = Path(os.path.realpath(here / args.rust_sysroot / "bin/rustc")).parent.parent

    # Resolved (/var -> /private/var): ld writes the debug map's object paths resolved, and -oso_prefix must match.
    work = Path(os.path.realpath(tempfile.mkdtemp(prefix="nvk-", dir=os.environ.get("TMPDIR"))))
    mesa = work / "mesa"
    build = work / "nvk-{}-build".format("ios" if args.platform == "ios" else "host")
    with tarfile.open(args.mesa) as tar:
        tar.extractall(mesa, filter="fully_trusted")
    for patch in args.patch:
        run(["/usr/bin/git", "apply", "-p1", "--whitespace=nowarn", str(here / patch)], cwd=mesa)
    tree = git_tree(str(mesa)).hex()
    print("mesa tree {} ({} patches)".format(tree, len(args.patch)), flush=True)
    if args.expect_tree and tree != args.expect_tree:
        sys.exit("the patched Mesa tree is {}, expected {}".format(tree, args.expect_tree))
    shutil.copytree(here / args.wraps / "packagecache", mesa / "subprojects/packagecache")
    abi = work / "abi"
    abi.mkdir()
    shutil.copyfile(args.abi_header, abi / Path(args.abi_header).name)

    bin_dir = work / "bin"
    bin_dir.mkdir()
    python = os.path.realpath(sys.executable)
    os.symlink(python, bin_dir / "python3.13")
    for name, target in (("ninja", args.ninja), ("bindgen", args.bindgen), ("cbindgen", args.cbindgen)):
        os.symlink(here / target, bin_dir / name)
    me = os.path.realpath(__file__)
    for tool in ("mesa_clc", "vtn_bindgen2", "glslangValidator"):
        write_script(bin_dir / tool, '#!/bin/sh\nexec {} {} stand-in {} "$@"\n'.format(python, me, tool))
    sdk = xcode["IPHONEOS_SDK"] if args.platform == "ios" else xcode["MACOSX_SDK"]
    env = {
        "PATH": os.pathsep.join([str(bin_dir), str(sysroot / "bin"), "/usr/bin", "/bin", "/usr/sbin", "/sbin"]),
        "PYTHONPATH": os.pathsep.join(str(here / p) for p in args.python_path),
        "DEVELOPER_DIR": xcode["DEVELOPER_DIR"],
        "HOME": str(work / "home"),
        "TMPDIR": str(work / "tmp"),
        "LC_ALL": "C",
        "LIBCLANG_PATH": str((here / args.libclang / "lib").resolve()),
        "MESA_GIT_SHA1_OVERRIDE": args.git_sha1,
        "NVK_PINNED": str((here / args.pinned).resolve()),
        "BINDGEN_EXTRA_CLANG_ARGS": "--target={} -isysroot {}".format(
            "arm64-apple-ios17.0" if args.platform == "ios" else "arm64-apple-macosx14.0", sdk),
    }
    (work / "home").mkdir()
    (work / "tmp").mkdir()
    remap = "--remap-path-prefix={}={}".format(sysroot, args.rust_src_prefix)
    # The linker's debug map names the Rust std objects inside libnvk_rs.a by absolute path, the build directory's:
    # relative to it instead, so the dylib (and its LC_UUID) do not depend on where the action ran. (r9's map named
    # the hand build's directory; that, and the LC_UUID ld derives from it, is all that differs from r9.)
    oso = "-Wl,-oso_prefix,{}/".format(build)
    setup = [python, "-m", "mesonbuild.mesonmain", "setup", str(build), "--wrap-mode=nodownload"] + MESON_OPTIONS + [
        "-Dnvk-darwin-abi-dir=" + str(abi)]
    if args.platform == "ios":
        for name, driver in (("ios-cc", "clang"), ("ios-c++", "clang++")):
            write_script(bin_dir / name, '#!/bin/sh\nexec xcrun -sdk iphoneos {} -target {} "$@"\n'.format(
                driver, IOS_TRIPLE))
        cross = work / "ios-cross.ini"
        cross.write_text("""\
[binaries]
c = '{cc}'
cpp = '{cxx}'
objc = '{cc}'
ar = ['xcrun', 'ar']
strip = ['xcrun', 'strip']
rust = ['rustc', '--target', 'aarch64-apple-ios']
pkg-config = ['false']
[built-in options]
c_args = ['-Wno-error=incompatible-function-pointer-types']
c_link_args = ['{oso}']
cpp_link_args = ['{oso}']
rust_args = ['{remap}']
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
""".format(cc=bin_dir / "ios-cc", cxx=bin_dir / "ios-c++", remap=remap, oso=oso))
        setup += ["--cross-file", str(cross)]
    else:
        setup += ["-Drust_args=" + remap, "-Dc_link_args=" + oso, "-Dcpp_link_args=" + oso]
    run(setup, cwd=mesa, env=env)
    run([python, "-m", "mesonbuild.mesonmain", "configure", str(build), "-Dnvk-build-id=" + args.build_id], env=env)
    run([str(bin_dir / "ninja"), "-C", str(build), TARGET], env=env)
    run([str(here / args.relink), str(build)], env=env)

    dylib = build / TARGET
    shutil.copyfile(dylib, args.out)
    Path(args.out_source).write_text("mesa-tree {}\nnvk-build-id {}\n".format(tree, args.build_id))
    digest = sha256(args.out)
    print("{}: {} (sha256 {})".format(args.platform, args.out, digest), flush=True)
    if args.expect_sha256 and digest != args.expect_sha256:
        sys.exit("libvulkan_nouveau.dylib has sha256 {}, expected {}".format(digest, args.expect_sha256))
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
