"""The one host input the build declares: Xcode, its toolchain and its iOS/macOS SDKs.

Apple's licence does not allow downloading or redistributing them, so the build cannot fetch them. Instead this
repository rule finds the Xcode named by $DEVELOPER_DIR (or `xcode-select -p`), checks it against the pins below,
and writes @xcode//:identity, a file every action that runs an Xcode tool takes as an input. Changing Xcode
therefore changes those actions' keys (and nothing else's), and an unpinned Xcode is refused before anything runs.

Bump: install the new Xcode, `xcode-select -s` it (or export DEVELOPER_DIR), run `bazel run @xcode//:print_pins`
and paste its output over XCODE_PINS. The kernel's code generation depends on Apple clang's version, so expect
every kernel object to rebuild.
"""

# Xcode-beta 27.0 (27A5194q): the Xcode the CMake flow built the hardware-tested kernels with.
XCODE_PINS = {
    "version": "27.0",
    "build": "27A5194q",
    "clang": "Apple clang version 21.0.0 (clang-2100.3.20.102)",
    "ld": "1328.2",
    "sha256": {
        "Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/SDKSettings.json": "cdeed07cd3d7a671a6b887347ccbad5ce221b38a3ecbcf90d23a52448ed6f311",
        "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/SDKSettings.json": "855868bc51c250cee55cccff0a6e5f6e7ef17856e921b44837dfec8c003f0693",
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/clang": "178ff35952fbf7651eaab02f8c07929f952b99f9eb54d0b8363080a98ce49029",
        "Toolchains/XcodeDefault.xctoolchain/usr/bin/ld": "eb49fa9579cc2a712451e1e6ab85837de02e90d455c8f5bd5efab4fba755a0b7",
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
        digests[relative] = _run(rctx, ["/usr/bin/shasum", "-a", "256", path]).split(" ")[0]

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
        print("WARNING: building with an UNPINNED Xcode ({}); outputs will not match the pinned build.".format(app))

    # The SDK paths as `xcrun --show-sdk-path` names them (iPhoneOS27.0.sdk): the spelling the CMake flow passed to
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
