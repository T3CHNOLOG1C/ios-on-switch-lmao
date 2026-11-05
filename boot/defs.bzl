"""//boot:package, the Hekate boot entry, one action per step. Each step reads only what it needs, so an edit reruns
only the steps it reaches:

    boot_args.py ............................ boot-args.txt
    overlay.py plan  (manifest, boot-args) .. overlay-plan.json
    adt_edit.py      (board ADT, plan) ...... ioswitch.adt
    overlay.py trust (plan, Mach-Os) ........ overlay.trustcache
    trust.py         (IPSW, overlay, GPU) ... trustcache.bundle
    build_native_loader.py layout (kernel, ADT, bundle, seed, boot-args)
                                         ...... plan.json, the window's 1 MiB LZ4 blocks in 96 shards (by the MiB
                                                each starts in)
    lz4 -12, one action per shard ........... each shard's blocks compressed and checked (an unchanged shard is a
                                               cache hit: a driver edit recompresses the native kernel's few blocks,
                                               a boot-arg, ADT or trust cache edit the tail's)
    build_native_loader.py join ............. layout.h, runs.h, kernel-window.bin, constants.json
    clang -c, one per loader source ......... loader objects (the window's constants only for those including them)
    loader_link.py ........................... loader.uboot.bin, boot.cmd, boot.scr (boot_script.py: the loader's
                                               size and bl33.bin's, which the window's constants give)
    bl33.py patch (once) / stage ............. bl33.bin
    overlay.py tree  (plan, overlay files) .. overlay/
    cp ...................................... ioswitch/ (the directory //deploy:sd, //install:sd and //boot:snapshot take)

So an overlay plist edit reruns the overlay tree and the final copy; a driver edit reruns the kernel's steps from the
layout on (the shards it changed, four loader objects, the link with the boot script, the bl33 stage and the copy); a
boot-arg edit reruns from the ADT edit on, but compresses only the window's tail. The Python steps run on the hermetic interpreter and need nothing of Xcode; the loader's compiles do (Xcode's
clang, the declared host input).
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

# Xcode's tools: macOS only, never on a remote executor (Xcode cannot be shipped there); the results may be cached
# remotely (the key includes @xcode//:identity).
_XCODE_EXECUTION = {"no-remote-exec": "", "requires-darwin": ""}

# The loader is freestanding AArch64 code entered by U-Boot's bootm with the MMU and caches off (boot/loader).
# PROBE_QUIET: the shared handoff probe draws nothing and skips its FDT CRC/walk unless it refuses (the loader checks
# the FDT itself); everything before the copy runs with the caches off.
LOADER_COPTS = [
    "--target=aarch64-none-elf",
    "-march=armv8-a",
    "-mstrict-align",
    "-mgeneral-regs-only",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-DPROBE_QUIET=1",
]

# The hekate entry. Every key also reaches U-Boot's environment (hekate's L4T launcher passes the section on; U-Boot
# imports it before its distro scan of boot_prefixes).
# bootwait=0: hekate always shows its boot logo before an L4T entry and, launched from Nyx, waits [config] bootwait
# seconds on it (3 by default; the entry's own key wins, main.c _auto_launch). Holding VOL- still opens hekate's menu.
# scan_dev_for_extlinux: the distro scan's extlinux.conf probe (this entry has none) becomes the first boot timing
# sample, taken once U-Boot, the SD card and hekate's env are up (boot/loader/stamps.h).
# bootcmd_mmc0=true: should boot.scr not take over, U-Boot's distro loop goes on to mmc0, the eMMC (initialize, read
# its partition table, scan it); never. bl33.py's patch also drops mmc0 from boot_targets for failures before hekate's
# env is imported (an SD card U-Boot cannot read).
_HEKATE_ENTRY = ("[IOSwitch]\nl4t=1\nboot_prefixes=/switchroot/{name}/\nid=IOSWITCH\nbootwait=0\n" +
                 "scan_dev_for_extlinux=setexpr.l ios_uboot *0x60005010\nbootcmd_mmc0=true\n")

# The shell that runs an Xcode tool: DEVELOPER_DIR from @xcode//:xcode.env (its first argument), then the tool.
_XCODE_SHELL = 'DEVELOPER_DIR="$(/usr/bin/sed -n "s/^DEVELOPER_DIR=//p" "$1")"; export DEVELOPER_DIR; shift; exec "$@"'

def _xcode(ctx, tool, arguments, inputs, outputs, mnemonic, message):
    ctx.actions.run_shell(
        command = _XCODE_SHELL,
        arguments = [ctx.file._xcode_env.path, tool] + arguments,
        inputs = depset(inputs + [ctx.file._xcode_env, ctx.file._xcode_identity]),
        outputs = outputs,
        env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin", "TZ": "UTC"},
        mnemonic = mnemonic,
        progress_message = message,
        execution_requirements = _XCODE_EXECUTION,
    )

def _run(ctx, tool, arguments, inputs, outputs, mnemonic, message):
    ctx.actions.run(
        executable = tool,
        arguments = arguments,
        inputs = inputs,
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = message,
    )

# The kernel window's shards (tools/package/loader_abi.py WINDOW_SHARDS: the MiB of the window a block starts in).
_WINDOW_SHARDS = 96

def _two_digits(i):
    return ("0" + str(i))[-2:]

# One shard: lz4 -12 over each of its blocks (none, one, or a few small ones), each checked by decoding it again.
_COMPRESS_SHARD = """set -e
lz4="$1"; in="$2"; out="$3"
for block in "$in"/*; do
    [ -f "$block" ] || continue
    name="${block##*/}"
    "$lz4" -12 -B6 --no-frame-crc -c -q "$block" > "$out/$name.lz4"
    "$lz4" -d -c -q "$out/$name.lz4" | /usr/bin/cmp -s - "$block" || { echo "lz4: $name does not decode to its block" >&2; exit 1; }
done
"""

def _one(target, what):
    files = target.files.to_list()
    if len(files) != 1:
        fail("{} {} must be one file, not {}".format(what, target.label, len(files)))
    return files[0]

def _boot_package_impl(ctx):
    name = ctx.attr.name

    def out(path):
        return ctx.actions.declare_file(name + "_work/" + path)

    smp = ctx.attr._smp[BuildSettingInfo].value

    # --- boot-args, overlay plan, ADT ------------------------------------------------------------------------------
    boot_args = out("boot-args.txt")
    _run(ctx, ctx.executable._boot_args, [boot_args.path] + (["--smp"] if smp else []), [], [boot_args], "BootArgs", "Writing the boot-args")

    # Manifest source -> file: the Mach-Os the trust module may name, then every other overlay file.
    binaries = {source: _one(target, "overlay binary") for target, source in ctx.attr.overlay_binaries.items()}
    files = dict(binaries)
    for target, source in ctx.attr.overlay_data.items():
        files[source] = _one(target, "overlay file")

    # Source files the manifest names by their path in the kernel tree (the manifest's ../): no mapping to keep.
    parent = ctx.file.overlay_manifest.short_path.rpartition("/")[0].rpartition("/")[0]
    tree = parent + "/" if parent else ""
    for f in ctx.files.overlay_files:
        if not f.short_path.startswith(tree):
            fail("overlay_files {} is not in the kernel tree {}".format(f.short_path, tree))
        files[f.short_path[len(tree):]] = f

    plan = out("overlay-plan.json")
    args = ctx.actions.args()
    args.add("plan")
    args.add("--manifest", ctx.file.overlay_manifest)
    args.add("--boot-args", boot_args)
    args.add("--portrait-header", ctx.file.portrait_header)
    args.add_all(sorted(files.keys()), before_each = "--given")
    args.add("--out", plan)
    _run(ctx, ctx.executable._overlay, [args], [ctx.file.overlay_manifest, boot_args, ctx.file.portrait_header], [plan], "OverlayPlan", "Planning the userland overlay")

    adt = out("ioswitch.adt")
    args = ctx.actions.args()
    args.add("--adt", ctx.file.adt)
    args.add("--boot-args", boot_args)
    args.add("--overlay-plan", plan)
    args.add("--bt-patch", ctx.file.bt_patch)
    args.add("--portrait-header", ctx.file.portrait_header)
    if smp:
        args.add("--smp")
    args.add("--out", adt)
    _run(ctx, ctx.executable._adt_edit, [args], [ctx.file.adt, boot_args, plan, ctx.file.bt_patch, ctx.file.portrait_header], [adt], "BootAdt", "Giving the ADT its iPad identity")

    # --- trust caches ---------------------------------------------------------------------------------------------
    overlay_trust = out("overlay.trustcache")
    args = ctx.actions.args()
    args.add("trust")
    args.add("--plan", plan)
    args.add_all(["%s=%s" % (source, f.path) for source, f in sorted(binaries.items())], before_each = "--file")
    args.add("--out", overlay_trust)
    _run(ctx, ctx.executable._overlay, [args], [plan] + binaries.values(), [overlay_trust], "OverlayTrust", "Hashing the overlay's code")

    gpu = ctx.attr._gpu_trustcache[DefaultInfo].files.to_list()
    if len(gpu) > 1:
        fail("the GPU trust cache must be one file")
    bundle = out("trustcache.bundle")
    args = ctx.actions.args()
    args.add("--static", ctx.file.static_trust)
    args.add("--app", ctx.file.app_trust)
    args.add_all([overlay_trust] + gpu, before_each = "--module")
    args.add("--out", bundle)
    _run(ctx, ctx.executable._trust, [args], [ctx.file.static_trust, ctx.file.app_trust, overlay_trust] + gpu, [bundle], "BootTrust", "Bundling the static trust caches")

    # --- the kernel window and the loader --------------------------------------------------------------------------
    # The prelinked kernel as linked: Xcode's strip and llvm-objcopy both refuse its layout ("string table not at the
    # end of the file"), so a release kernel is stripped where it is linked, before the prelinker (which binds the kexts
    # to its global symbols, all a strip -x keeps).
    kernel = ctx.file.kernel
    # Layout and checks, then each shard of the window's LZ4 blocks compressed on its own (only the shards an edit
    # changed rerun), then the staged window and the loader's headers.
    window_plan = out("window/plan.json")
    shards = [ctx.actions.declare_directory(name + "_work/window/blocks/" + _two_digits(i)) for i in range(_WINDOW_SHARDS)]
    args = ctx.actions.args()
    args.add("layout")
    args.add("--kernel", kernel)
    args.add("--adt", adt)
    args.add("--trustcache", bundle)
    args.add("--data-seed", ctx.file.data_seed)
    args.add("--boot-args", boot_args)
    args.add("--plan", window_plan)
    args.add("--blocks", shards[0].dirname)
    _run(ctx, ctx.executable._window, [args], [kernel, adt, bundle, ctx.file.data_seed, boot_args], [window_plan] + shards, "KernelLayout", "Laying out the kernel window")

    compressed = []
    for i, shard in enumerate(shards):
        lz4 = ctx.actions.declare_directory(name + "_work/window/lz4/" + _two_digits(i))
        ctx.actions.run_shell(
            command = _COMPRESS_SHARD,
            arguments = [ctx.file._lz4.path, shard.path, lz4.path],
            inputs = [shard],
            tools = [ctx.file._lz4],
            outputs = [lz4],
            env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
            mnemonic = "KernelCompress",
            progress_message = "Compressing the kernel window's MiB %d" % i,
        )
        compressed.append(lz4)

    layout_h, runs_h, window, constants = out("gen/layout.h"), out("gen/runs.h"), out("kernel-window.bin"), out("constants.json")
    args = ctx.actions.args()
    args.add("join")
    args.add("--plan", window_plan)
    args.add("--compressed", compressed[0].dirname)
    args.add("--layout-h", layout_h)
    args.add("--runs-h", runs_h)
    args.add("--window", window)
    args.add("--constants", constants)
    _run(ctx, ctx.executable._window, [args], [window_plan] + compressed, [layout_h, runs_h, window, constants], "KernelWindow", "Staging the kernel window")

    includes = []
    for marker in ctx.files.loader_include_markers:
        includes += ["-I", marker.dirname]
    windowed = {f.path: True for f in ctx.files.loader_window_srcs}
    objects = []
    for i, src in enumerate(ctx.files.loader_srcs):
        obj = out("loader/part-%d.o" % i)
        reads_window = src.path in windowed
        _xcode(
            ctx,
            "/usr/bin/clang",
            LOADER_COPTS + (["-I", layout_h.dirname] if reads_window else []) + includes + ["-c", src.path, "-o", obj.path],
            [src] + ctx.files.loader_hdrs + ([layout_h, runs_h] if reads_window else []),
            [obj],
            "LoaderCompile",
            "Compiling the loader's %s" % src.short_path,
        )
        objects.append(obj)
    for src in ctx.files.loader_window_srcs:
        if src not in ctx.files.loader_srcs:
            fail("loader_window_srcs {} is not in loader_srcs".format(src.short_path))

    loader, cmd, scr = out("loader.uboot.bin"), out("boot.cmd"), out("boot.scr")
    args = ctx.actions.args()
    args.add("--ld-lld", ctx.file._ld_lld)
    args.add("--objcopy", ctx.file._objcopy)
    args.add("--readelf", ctx.file._readelf)
    args.add("--linker-script", ctx.file.linker_script)
    args.add("--constants", constants)
    args.add("--out", loader)
    # boot.cmd and boot.scr need only the loader's size and the window's constants (bl33.bin's size follows from them).
    args.add("--boot-template", ctx.file.template)
    args.add("--boot-cmd", cmd)
    args.add("--boot-scr", scr)
    args.add_all(objects)
    ctx.actions.run(
        executable = ctx.executable._loader_link,
        arguments = [args],
        inputs = objects + [ctx.file.linker_script, constants, ctx.file.template],
        tools = [ctx.file._ld_lld, ctx.file._objcopy, ctx.file._readelf],
        outputs = [loader, cmd, scr],
        mnemonic = "LoaderLink",
        progress_message = "Linking the native loader",
    )

    # --- U-Boot and its script ------------------------------------------------------------------------------------
    u_boot = out("u-boot.bin")
    _run(ctx, ctx.executable._bl33, ["patch", ctx.file.bl33.path, u_boot.path], [ctx.file.bl33], [u_boot], "BootUBoot", "Patching U-Boot's default environment")
    bl33 = out("bl33.bin")
    args = ctx.actions.args()
    args.add("stage")
    args.add("--u-boot", u_boot)
    args.add("--window", window)
    args.add("--constants", constants)
    args.add("--out", bl33)
    _run(ctx, ctx.executable._bl33, [args], [u_boot, window, constants], [bl33], "BootBl33", "Staging the kernel window in bl33.bin")

    entry = ctx.attr.entry
    ini = out("%s.ini" % entry)
    ctx.actions.write(ini, _HEKATE_ENTRY.format(name = entry))

    # --- the overlay and the directory -----------------------------------------------------------------------------
    overlay = ctx.actions.declare_directory(name + "_work/overlay")
    args = ctx.actions.args()
    args.add("tree")
    args.add("--plan", plan)
    args.add_all(["%s=%s" % (source, f.path) for source, f in sorted(files.items())], before_each = "--file")
    args.add("--out", overlay.path)
    _run(ctx, ctx.executable._overlay, [args], [plan] + files.values(), [overlay], "OverlayTree", "Laying out the userland overlay")

    switchroot = {
        "bl31.bin": ctx.file.bl31,
        "bl33.bin": bl33,
        "boot.cmd": cmd,
        "boot.scr": scr,
        "exporter.uimg": ctx.file.exporter,
        "loader.uboot.bin": loader,
        "oled-sku3.dtb": ctx.file.dtb,
    }
    package = ctx.actions.declare_directory(ctx.attr.out)
    commands = [
        'set -e; out="$1"',
        '/bin/mkdir -p "$out/switchroot/%s" "$out/bootloader/ini"' % entry,
        '/bin/cp -R -L "%s" "$out/overlay"' % overlay.path,
        '/bin/cp -L "%s" "$out/bootloader/ini/%s.ini"' % (ini.path, entry),
    ] + [
        '/bin/cp -L "%s" "$out/switchroot/%s/%s"' % (f.path, entry, n)
        for n, f in sorted(switchroot.items())
    ]
    ctx.actions.run_shell(
        command = "\n".join(commands),
        arguments = [package.path],
        inputs = [overlay, ini] + switchroot.values(),
        outputs = [package],
        mnemonic = "BootPackage",
        progress_message = "Assembling the IOSwitch boot entry",
    )
    return [
        DefaultInfo(files = depset([package])),
        OutputGroupInfo(
            adt = depset([adt]),
            kernel_window = depset([window, layout_h, runs_h, constants, window_plan]),
            loader = depset([loader]),
            overlay = depset([plan, overlay_trust, overlay]),
            trust = depset([bundle]),
        ),
    ]

def _tool(label):
    return attr.label(default = label, executable = True, cfg = "exec")

boot_package = rule(
    implementation = _boot_package_impl,
    doc = "The Hekate boot entry (switchroot/<entry>/*, bootloader/ini/<entry>.ini, overlay/<absolute path>) from the " +
          "prelinked kernel, the IPSW's trust caches, the GPU trust cache, the overlay and the native loader's sources.",
    attrs = {
        "out": attr.string(default = "ioswitch"),
        "entry": attr.string(default = "ioswitch", doc = "the entry's folder (switchroot/<entry>/) and ini name"),
        "kernel": attr.label(mandatory = True, allow_single_file = True),
        "adt": attr.label(mandatory = True, allow_single_file = True, doc = "the board ADT (//boot:adt)"),
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
        "overlay_binaries": attr.label_keyed_string_dict(allow_files = True, doc = "built Mach-O -> its manifest source"),
        "overlay_data": attr.label_keyed_string_dict(allow_files = True, doc = "other overlay file -> its manifest source"),
        "overlay_files": attr.label_list(allow_files = True, doc = "overlay files the manifest names by tree path"),
        "loader_srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True, doc = "in link order"),
        "loader_window_srcs": attr.label_list(allow_files = True, doc = "those of loader_srcs that include layout.h/runs.h"),
        "loader_hdrs": attr.label_list(allow_files = True),
        "loader_include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory, in order"),
        "linker_script": attr.label(mandatory = True, allow_single_file = True),
        "_gpu_trustcache": attr.label(default = "//boot:gpu_trustcache"),
        "_smp": attr.label(default = "//boot:smp"),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
        "_boot_args": _tool("//tools/package:boot_args"),
        "_overlay": _tool("//tools/package:overlay"),
        "_adt_edit": _tool("//tools/package:adt_edit"),
        "_trust": _tool("//tools/package:trust"),
        "_window": _tool("//tools/package:window"),
        "_loader_link": _tool("//tools/package:loader_link"),
        "_bl33": _tool("//tools/package:bl33"),
        # The kernel window's compressor (lz4 -12, one action per shard), built from pinned source; LLVM 23.1.4's
        # linker and binutils.
        "_lz4": attr.label(default = "//tools:lz4", cfg = "exec", allow_single_file = True),
        "_ld_lld": attr.label(default = "@llvm//:bin/ld.lld", cfg = "exec", allow_single_file = True),
        "_objcopy": attr.label(default = "@llvm//:bin/llvm-objcopy", cfg = "exec", allow_single_file = True),
        "_readelf": attr.label(default = "@llvm//:bin/llvm-readelf", cfg = "exec", allow_single_file = True),
    },
)
