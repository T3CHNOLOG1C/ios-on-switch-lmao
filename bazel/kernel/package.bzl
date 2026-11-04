"""The boot entry: the userland overlay's iOS binaries and the package (tools/package/boot.py)."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "run_tool", "setting")

def _ios_binary_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    args = [
        "--xcode-env",
        ctx.file._xcode_env.path,
        "--out",
        out.path,
        "--path-root",
        setting(ctx, "_path_root"),
        "--bin-dir",
        ctx.bin_dir.path,
    ]
    if ctx.attr.coreaudio_headers:
        args.append("--coreaudio-headers")
    if ctx.attr.sign_identifier:
        args += ["--sign-identifier", ctx.attr.sign_identifier]
    if ctx.file.entitlements:
        args += ["--entitlements", ctx.file.entitlements.path]
    if ctx.attr.pagesize:
        args += ["--pagesize", ctx.attr.pagesize]
    args.append("--")
    for flag in ctx.attr.copts:
        args.append(flag.replace("{tree}", ctx.label.package))
    args += [f.path for f in ctx.files.srcs]
    run_tool(
        ctx,
        "ios_binary.py",
        args,
        depset(ctx.files.srcs + ctx.files.hdrs + ([ctx.file.entitlements] if ctx.file.entitlements else [])),
        [out],
        "IosBinary",
        "Building " + ctx.attr.out,
    )
    return [DefaultInfo(files = depset([out]))]

ios_binary = rule(
    implementation = _ios_binary_impl,
    doc = "An ad hoc signed arm64 iOS 17 binary (the overlay's tools, plug-ins and decoders).",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(mandatory = True),
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "hdrs": attr.label_list(allow_files = True),
        "copts": attr.string_list(),
        "coreaudio_headers": attr.bool(),
        "sign_identifier": attr.string(),
        "entitlements": attr.label(allow_single_file = True),
        "pagesize": attr.string(),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _boot_package_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.out)
    gpu = ctx.attr._gpu_trustcache[DefaultInfo].files.to_list()
    args = [
        "--xcode-env",
        ctx.file._xcode_env.path,
        "--kernel",
        ctx.file.kernel.path,
        "--adt",
        ctx.file.adt.path,
        "--static-trust",
        ctx.file.static_trust.path,
        "--app-trust",
        ctx.file.app_trust.path,
        "--data-seed",
        ctx.file.data_seed.path,
        "--template",
        ctx.file.template.path,
        "--bl31",
        ctx.file.bl31.path,
        "--bl33",
        ctx.file.bl33.path,
        "--dtb",
        ctx.file.dtb.path,
        "--exporter",
        ctx.file.exporter.path,
        "--bt-patch",
        ctx.file.bt_patch.path,
        "--overlay-manifest",
        ctx.file.overlay_manifest.path,
        "--portrait-header",
        ctx.file.portrait_header.path,
        "--out",
        out.path,
    ]
    if len(gpu) > 1:
        fail("the GPU trust cache must be one file")
    if gpu:
        args += ["--gpu-trust", gpu[0].path]
    overlay = []
    for target, source in ctx.attr.overlay.items():
        files = target.files.to_list()
        if len(files) != 1:
            fail("overlay {} must be one file".format(target.label))
        args += ["--overlay", source + "=" + files[0].path]
        overlay += files

    # Source files the manifest names by their path in the kernel tree (the manifest's ../): no mapping to keep.
    tree = ctx.file.overlay_manifest.short_path.rpartition("/")[0].rpartition("/")[0] + "/"
    for f in ctx.files.overlay_files:
        if not f.short_path.startswith(tree):
            fail("overlay_files {} is not in the kernel tree {}".format(f.short_path, tree))
        args += ["--overlay", f.short_path[len(tree):] + "=" + f.path]
        overlay.append(f)
    for tool in ctx.files._llvm_tools:
        args += ["--llvm-tool", tool.path]
    for source in ctx.files._loader_sources + ctx.files._package_tools:
        args += ["--source", source.path]
    if ctx.attr._smp[BuildSettingInfo].value:
        args.append("--smp")
    inputs = [
        ctx.file.kernel,
        ctx.file.adt,
        ctx.file.static_trust,
        ctx.file.app_trust,
        ctx.file.data_seed,
        ctx.file.template,
        ctx.file.bl31,
        ctx.file.bl33,
        ctx.file.dtb,
        ctx.file.exporter,
        ctx.file.bt_patch,
        ctx.file.overlay_manifest,
        ctx.file.portrait_header,
    ] + gpu + overlay + ctx.files._loader_sources + ctx.files._package_tools + ctx.files._llvm_tools
    run_tool(
        ctx,
        ctx.attr._boot_tool,
        args,
        depset(inputs),
        [out],
        "BootPackage",
        "Packaging the IOSwitch boot entry",
    )
    return [DefaultInfo(files = depset([out]))]

boot_package = rule(
    implementation = _boot_package_impl,
    doc = "The Hekate boot entry (out/boot/ioswitch's layout: switchroot/ioswitch/*, bootloader/ini/ioswitch.ini, " +
          "overlay/<path>) from the prelinked kernel, the IPSW trust caches, the GPU trust cache and the overlay.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(default = "ioswitch"),
        "kernel": attr.label(mandatory = True, allow_single_file = True),
        "adt": attr.label(mandatory = True, allow_single_file = True),
        "static_trust": attr.label(mandatory = True, allow_single_file = True),
        "app_trust": attr.label(mandatory = True, allow_single_file = True),
        "data_seed": attr.label(mandatory = True, allow_single_file = True),
        "template": attr.label(mandatory = True, allow_single_file = True),
        "bl31": attr.label(mandatory = True, allow_single_file = True),
        "bl33": attr.label(mandatory = True, allow_single_file = True),
        "dtb": attr.label(mandatory = True, allow_single_file = True),
        "exporter": attr.label(mandatory = True, allow_single_file = True),
        "bt_patch": attr.label(mandatory = True, allow_single_file = True),
        "overlay_manifest": attr.label(mandatory = True, allow_single_file = True),
        "portrait_header": attr.label(mandatory = True, allow_single_file = True),
        "overlay": attr.label_keyed_string_dict(allow_files = True, doc = "built overlay file -> its manifest source"),
        "overlay_files": attr.label_list(allow_files = True, doc = "overlay files the manifest names by tree path"),
        "_gpu_trustcache": attr.label(default = "//boot:gpu_trustcache"),
        "_smp": attr.label(default = "//boot:smp"),
        "_boot_tool": attr.label(default = "//tools/package:boot", executable = True, cfg = "exec"),
        "_package_tools": attr.label(default = "//tools/package:sources"),
        "_loader_sources": attr.label(default = "//native:loader_sources"),
        "_llvm_tools": attr.label_list(
            default = [
                "@llvm//:bin/ld.lld",
                "@llvm//:bin/llvm-objcopy",
                "@llvm//:bin/llvm-readelf",
                "@llvm//:bin/llvm-objdump",
                # The kernel window's compressor (lz4 -12), built from pinned source.
                "//tools:lz4",
            ],
            allow_files = True,
            cfg = "exec",
        ),
    }),
    toolchains = [PY_TOOLCHAIN],
)
