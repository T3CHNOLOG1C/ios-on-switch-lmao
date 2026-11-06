"""hekate payloads: freestanding armv4t Thumb code for the BPMP, built with @arm_none_eabi the way hekate's Makefile
builds hekate (its flags, its start.S and link.ld at IPL_LOAD_ADDR), one compile action per source."""

ARCH = ["-march=armv4t", "-mtune=arm7tdmi", "-mthumb", "-mthumb-interwork"]

COPTS = ARCH + [
    "-Os",
    "-nostdlib",
    "-ffunction-sections",
    "-fdata-sections",
    "-fomit-frame-pointer",
    "-std=gnu11",
    "-Wall",
    "-Wno-array-bounds",
    "-DIPL_LOAD_ADDR=0x40008000",
    "-DBL_MAGIC=0x43544349",
    "-DBDK_MALLOC_NO_DEFRAG",
    '-DGFX_INC="gfx.h"',
    '-DFFCFG_INC="ffconf.h"',
]

# hekate's BDK sources assume devkitARM's newlib, which brings <stdint.h> in through its own headers.
C_ONLY = ["-include", "stdint.h"]

def _gcc(ctx):
    return ctx.executable._gcc

def _payload_impl(ctx):
    hekate = ctx.attr._hekate_root.label.workspace_root
    includes = [hekate + "/bdk", ctx.label.package, hekate + "/bootloader"]
    headers = ctx.files.hdrs + ctx.files._hekate_headers
    objects = []
    for src in ctx.files.srcs + ctx.files._hekate_srcs:
        obj = ctx.actions.declare_file("{}_objs/{}/{}.o".format(ctx.label.name, src.dirname.replace("/", "_"), src.basename))
        args = ctx.actions.args()
        args.add_all(COPTS)
        if src.extension == "c":
            args.add_all(C_ONLY)
        if not src.owner.workspace_name:
            args.add("-Werror")
        args.add_all(includes, before_each = "-I")
        args.add("-c", src)
        args.add("-o", obj)
        ctx.actions.run(
            executable = _gcc(ctx),
            arguments = [args],
            inputs = depset([src] + headers),
            tools = ctx.files._toolchain,
            outputs = [obj],
            mnemonic = "BpmpCompile",
            progress_message = "Compiling %s for the BPMP" % src.short_path,
            env = {"PATH": "/usr/bin:/bin"},
        )
        objects.append(obj)
    elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    args = ctx.actions.args()
    args.add_all(ARCH)
    args.add_all(["-nostartfiles", "-Wl,--nmagic,--gc-sections,--no-warn-rwx-segments,--fatal-warnings"])
    args.add_all(["-Wl,--wrap=" + symbol for symbol in ctx.attr.wrap])
    args.add("-Wl,--defsym=IPL_LOAD_ADDR=0x40008000")
    args.add("-T", ctx.file._link_script)
    args.add("-o", elf)
    args.add_all(objects)
    args.add_all(["-lgcc", "-lc"])
    ctx.actions.run(
        executable = _gcc(ctx),
        arguments = [args],
        inputs = objects + [ctx.file._link_script],
        tools = ctx.files._toolchain,
        outputs = [elf],
        mnemonic = "BpmpLink",
        progress_message = "Linking %{output}",
        env = {"PATH": "/usr/bin:/bin"},
    )
    out = ctx.outputs.out
    ctx.actions.run(
        executable = ctx.executable._objcopy,
        arguments = ["-S", "-O", "binary", elf.path, out.path],
        inputs = [elf],
        tools = ctx.files._toolchain,
        outputs = [out],
        mnemonic = "BpmpObjcopy",
        progress_message = "Writing %{output}",
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(elf = depset([elf]))]

hekate_payload = rule(
    implementation = _payload_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".c"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "out": attr.output(mandatory = True),
        "wrap": attr.string_list(doc = "symbols linked with --wrap (the payload defines __wrap_<symbol>)"),
        "_hekate_srcs": attr.label(default = ":bdk_sources"),
        "_hekate_headers": attr.label(default = "@hekate//:all"),
        "_hekate_root": attr.label(default = "@hekate//:Makefile", allow_single_file = True),
        "_link_script": attr.label(default = "@hekate//:bootloader/link.ld", allow_single_file = True),
        "_toolchain": attr.label(default = "@arm_none_eabi//:all"),
        "_gcc": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-gcc", executable = True, allow_single_file = True, cfg = "exec"),
        "_objcopy": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-objcopy", executable = True, allow_single_file = True, cfg = "exec"),
    },
    doc = "A hekate payload (.bin) from these sources and the BDK parts in :bdk_sources.",
)
