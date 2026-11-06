"""The one host input the build declares: Xcode, its toolchain and its iOS/macOS SDKs.

Apple's licence does not allow downloading or redistributing them, so the build cannot fetch them. Instead this
repository rule finds the Xcode named by $DEVELOPER_DIR (or `xcode-select -p`), checks it against the pins below,
and writes @xcode//:identity, a file every action that runs an Xcode tool takes as an input. Changing Xcode
therefore changes those actions' keys (and nothing else's), and an unpinned Xcode is refused before anything runs.

Bump: install the new Xcode, `xcode-select -s` it (or export DEVELOPER_DIR), run `bazel run @xcode//:print_pins`
and paste its output over XCODE_PINS, then re-record what XNU's make does with it (`bazel run
//kernel/xnu-<version>:update_tables` for each XNU, `bazel run //bazel/toolchains/kernel:update_recipes`). The
kernel's code generation depends on Apple clang's version, so expect every kernel object to rebuild.
"""

# CI selects the Xcode with this build: GitHub's xcode-27 runner image must keep it (/Applications/Xcode_27.2.app).
XCODE_PINS = {
    "version": "27.2",
    "build": "27B52",
    "clang": "Apple clang version 21.0.0 (clang-2100.4.1.1)",
    "ld": "27106.1",
    "sha256": {
        "Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/SDKSettings.json": "8c5d2efa53dbdb9b602c45e77c5eac5b21165317408fe37ddf14c7aa9c8b9031",
        "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/SDKSettings.json": "82165b15ee21419d4cffaba8895797391b908ac9499ba70f561f3e1bd16758d6",
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/clang": "fc5e3cbba6ad54059295cad716aacb05ace8235f5f7a6031f1d8c15d24bd6d66",
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/ld": "7f7041b1bb30e5e6a815a55edf374065d5c82e1ab93f84d5b6ed5656ded24bcf",
    },
}

_BUILD = """\
package(default_visibility = ["//visibility:public"])

exports_files(["identity", "xcode.env"])

# `bazel run @xcode//:print_pins`: the XCODE_PINS for the selected Xcode (to bump the pin).
sh_binary(
    name = "print_pins",
    srcs = ["print_pins.sh"],
)
"""

def _run(rctx, args, env = {}):
    result = rctx.execute(args, environment = env, quiet = True)
    if result.return_code:
        fail("{} failed ({}): {}".format(" ".join(args), result.return_code, result.stderr))
    return result.stdout.strip()

# A universal Mach-O is hashed by its arm64 slice, the code that runs here: the App Store's Xcode ships the toolchain
# thinned to arm64, while Apple's .xip, which GitHub's runner images install, ships it universal around the same slice.
def _arm64_digest(rctx, path):
    archs = rctx.execute(["/usr/bin/lipo", "-archs", path], quiet = True)
    if archs.return_code == 0 and len(archs.stdout.strip().split(" ")) > 1:
        slice = str(rctx.path("arm64-slice"))
        _run(rctx, ["/usr/bin/lipo", path, "-thin", "arm64", "-output", slice])
        digest = _run(rctx, ["/usr/bin/shasum", "-a", "256", slice]).split(" ")[0]
        rctx.delete(slice)
        return digest
    return _run(rctx, ["/usr/bin/shasum", "-a", "256", path]).split(" ")[0]

def _xcode_host_impl(rctx):
    developer = rctx.getenv("DEVELOPER_DIR") or _run(rctx, ["/usr/bin/xcode-select", "-p"])
    if not developer.endswith("/Contents/Developer"):
        fail("DEVELOPER_DIR={} is not an Xcode (Command Line Tools have no iOS SDK)".format(developer))
    app = developer[:-len("/Contents/Developer")]
    plist = app + "/Contents/version.plist"
    rctx.watch(plist)
    version = _run(rctx, ["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleShortVersionString", plist])
    build = _run(rctx, ["/usr/libexec/PlistBuddy", "-c", "Print :ProductBuildVersion", plist])
    toolchain = developer + "/Toolchains/XcodeDefault.xctoolchain/usr/bin"
    clang = _run(rctx, [toolchain + "/clang", "--version"]).splitlines()[0]
    ld = ""
    for line in _run(rctx, [toolchain + "/ld", "-version_details"]).splitlines():
        if '"version"' in line:
            ld = line.split(":")[1].strip().strip(",").strip('"')
            break
    digests = {}
    for relative in sorted(XCODE_PINS["sha256"]):
        path = developer + "/" + relative
        rctx.watch(path)
        digests[relative] = _arm64_digest(rctx, path)

    found = {"version": version, "build": build, "clang": clang, "ld": ld, "sha256": digests}
    mismatches = [k for k in found if found[k] != XCODE_PINS[k]]
    pins = json.encode_indent(found, indent = "    ")
    if mismatches and rctx.getenv("SWITCH_ALLOW_UNPINNED_XCODE") != "1":
        fail(("\n\nThe selected Xcode ({}) does not match the pin in bazel/host/xcode.bzl ({} differ).\n" +
              "Select the pinned Xcode {} ({}) with `sudo xcode-select -s <Xcode.app>` or export DEVELOPER_DIR,\n" +
              "or bump the pin to this Xcode (every kernel object rebuilds):\n\nXCODE_PINS = {}\n").format(
            app,
            ", ".join(mismatches),
            XCODE_PINS["version"],
            XCODE_PINS["build"],
            pins,
        ))
    if mismatches:
        # buildifier: disable=print
        print("WARNING: building with an UNPINNED Xcode ({}); outputs will not match the pinned build.".format(app))

    # The SDK paths as `xcrun --show-sdk-path` names them (iPhoneOS27.2.sdk): the spelling the CMake flow passed to
    # -isysroot, which ends up in debug info.
    xcrun_env = {"DEVELOPER_DIR": developer}
    iphoneos = _run(rctx, ["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"], xcrun_env)
    macosx = _run(rctx, ["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], xcrun_env)
    rctx.file("identity", "\n".join([
        "developer_dir " + developer,
        "version " + version,
        "build " + build,
        "clang " + clang,
        "ld " + ld,
    ] + ["sha256 {} {}".format(digests[k], k) for k in sorted(digests)]) + "\n")

    # Sourced by the build's action wrappers (bazel/tools/xcode_env.sh): the absolute host paths, never PATH lookups.
    rctx.file("xcode.env", "\n".join([
        "DEVELOPER_DIR=" + developer,
        "XCODE_TOOLCHAIN=" + toolchain,
        "IPHONEOS_SDK=" + iphoneos,
        "MACOSX_SDK=" + macosx,
    ]) + "\n")
    rctx.file("print_pins.sh", "#!/bin/sh\ncat <<'EOF'\nXCODE_PINS = {}\nEOF\n".format(pins), executable = True)
    rctx.file("defs.bzl", "\n".join([
        "DEVELOPER_DIR = {}".format(repr(developer)),
        "XCODE_TOOLCHAIN = {}".format(repr(toolchain)),
        "IPHONEOS_SDK = {}".format(repr(iphoneos)),
        "MACOSX_SDK = {}".format(repr(macosx)),
        "XCODE_BUILD = {}".format(repr(build)),
    ]) + "\n")
    rctx.file("BUILD.bazel", 'load("@rules_shell//shell:sh_binary.bzl", "sh_binary")\n\n' + _BUILD)

xcode_host = repository_rule(
    implementation = _xcode_host_impl,
    environ = ["DEVELOPER_DIR", "SWITCH_ALLOW_UNPINNED_XCODE"],
    local = True,
    doc = "Finds and checks the host Xcode (the build's only undownloadable input).",
)
