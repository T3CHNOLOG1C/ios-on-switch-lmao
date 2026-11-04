"""Code linked into the kernel: compile recipes taken from XNU's own compile records, drivers, archives.

Adding a driver is one call in out/native-a57/root-v1/tree/BUILD.bazel:

    kernel_driver(name = "SwitchFoo")                     # drivers/SwitchFoo/*.cpp, IOKit C++ recipe, our warnings

plus its name in the member list of the archive it rides in (archive member order is link order: -force_load).
Knobs: srcs (ordered; .cpp/.c/.S, defaults to the directory's .cpp files), late (static initializer run by the board
code after the original family loads), abi_checks (SwitchUSB-style checks against the original kernelcache),
firmware (a firmware.S that .incbins the directory's firmware/), copts, c_srcs recipe, hdrs, prelink (original
kexts this driver needs prelinked: they join the prelink roots).
"""

load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "XCODE_EXECUTION", "run_tool", "setting", "workspace_root")
load(":xnu.bzl", "XnuInfo")

KernelObjectsInfo = provider(
    doc = "Objects in link order, and the original kexts they need prelinked.",
    fields = ["objects", "prelink", "cuts", "deferred"],
)

RecipeInfo = provider(fields = ["file"])

# cmake/Drivers.cmake IOSWITCH_DRIVER_WARNINGS: what every IOKit C++ driver adds to the kernel's flags.
DRIVER_WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-c99-designator",
    "-Wno-reorder-init-list",
    "-Wno-deprecated-enum-enum-conversion",
    "-Wno-unnecessary-virtual-specifier",
]

# ioswitch_bindings / firmware .incbin units: Xcode's clang for iOS 17, nothing from XNU's recipes.
PLAIN_ASM = ["xcrun", "clang", "-target", "arm64-apple-ios17.0", "-isysroot", "{sdk}", "-Werror"]

def _kernel_recipe_impl(ctx):
    out = ctx.actions.declare_file("recipes/" + ctx.label.name + ".json")
    xnu = ctx.attr.xnu[XnuInfo]
    args = ["--objroot", xnu.objroot.path, "--record", ctx.attr.record, "--out", out.path]
    if ctx.attr.keep_warnings:
        args.append("--keep-warnings")
    for prefix in ctx.attr.drop:
        args.append("--drop=" + prefix)
    if ctx.attr.strip_includes:
        args.append("--strip-includes")
    run_tool(ctx, "recipes.py", args, depset([xnu.objroot]), [out], "KernelRecipe", "Recipe " + ctx.label.name,
             execution_requirements = {})
    return [DefaultInfo(files = depset([out])), RecipeInfo(file = out)]

kernel_recipe = rule(
    implementation = _kernel_recipe_impl,
    doc = "A compile recipe from one of XNU's compile records (cmake/XnuRecipes.cmake ioswitch_recipe).",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "xnu": attr.label(mandatory = True, providers = [XnuInfo]),
        "record": attr.string(mandatory = True),
        "keep_warnings": attr.bool(),
        "drop": attr.string_list(),
        "strip_includes": attr.bool(),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _compile(ctx, src, obj, recipe, flags, command, extra_inputs):
    xnu = ctx.attr.xnu[XnuInfo]
    args = [
        "--xcode-env",
        ctx.file._xcode_env.path,
        "--tree",
        ctx.label.package,
        "--build",
        ctx.bin_dir.path + "/" + ctx.label.package + "/build",
        "--xnu",
        workspace_root(ctx.attr._xnu_src),
        "--path-root",
        setting(ctx, "_path_root"),
        "--bin-dir",
        ctx.bin_dir.path,
        "--source",
        src.path,
        "--output",
        obj.path,
    ]
    inputs = [src, xnu.objroot, xnu.sdkroot] + extra_inputs
    if recipe:
        args += ["--recipe", recipe.path]
        inputs.append(recipe)
    for c in command:
        args.append("--command=" + c)
    for f in flags:
        args.append("--flag=" + f)
    run_tool(
        ctx,
        "compile.py",
        args,
        depset(inputs, transitive = [depset(ctx.files._xnu_src)]),
        [obj],
        "KernelCompile",
        "Compiling " + src.short_path,
    )

def _stem(f):
    return f.basename[:f.basename.rfind(".")]

def _kernel_driver_impl(ctx):
    out_dir = ctx.attr.out_dir or ("build/" + ctx.label.name)
    hdrs = ctx.files.hdrs + ctx.files._common_hdrs
    late = {name: True for name in ctx.attr.late}
    checks = {}
    for bindings, spec in ctx.attr.abi_checks.items():
        cls, source = spec.split(" ")
        checks[source] = struct(cls = cls, bindings = bindings.files.to_list()[0])
    objects = []
    for src in ctx.files.srcs:
        ext = src.extension
        stem = _stem(src)
        if ext == "c" and ctx.attr.object_names == "path":
            # cmake/Drivers.cmake names vendored C objects by their path below the driver (nouveau has many base.c).
            stem = ctx.attr.object_prefix + src.short_path[len(ctx.attr.dir) + 1:].replace("/", "_")
        stem = ctx.attr.stems.get(src.basename, stem)
        if not stem:
            fail("empty stem for {} (dir {}, short_path {})".format(src, ctx.attr.dir, src.short_path))
        obj = ctx.actions.declare_file(out_dir + "/" + stem + ".o")
        extra = ctx.attr.src_copts.get(src.basename, [])
        if ext in ("cpp", "cc", "mm"):
            _compile(ctx, src, obj, ctx.attr.recipe[RecipeInfo].file,
                     (DRIVER_WARNINGS if ctx.attr.warnings else []) +
                     (["-I", "{xnu}/osfmk"] if ctx.attr.include_osfmk else []) + ctx.attr.copts + extra,
                     [], hdrs)
        elif ext == "c":
            if ctx.attr.c_command:
                _compile(ctx, src, obj, None, ctx.attr.c_copts + extra, ctx.attr.c_command, hdrs)
            else:
                _compile(ctx, src, obj, ctx.attr.c_recipe[RecipeInfo].file, ctx.attr.c_copts + extra, [], hdrs)
        elif ext in ("S", "s"):
            copts = list(ctx.attr.asm_copts)
            if src.basename == "firmware.S":
                copts += ["-I", "{tree}/" + src.dirname[len(ctx.label.package) + 1:] + "/firmware"]
            _compile(ctx, src, obj, None, copts, PLAIN_ASM, hdrs)
        else:
            fail("{}: unsupported source {}".format(ctx.label, src.basename))
        final = obj
        if src.basename in late:
            # cmake/XnuRecipes.cmake ioswitch_late_object.
            late_obj = ctx.actions.declare_file(out_dir + "/" + stem + ".late.o")
            ctx.actions.run(
                executable = ctx.executable._objcopy,
                arguments = [
                    "--remove-section=__DATA,__mod_init_func",
                    "--globalize-symbol=__GLOBAL__sub_I_" + src.basename,
                    obj.path,
                    late_obj.path,
                ],
                inputs = [obj],
                outputs = [late_obj],
                mnemonic = "KernelLateObject",
                progress_message = "Staging the static initializer of " + src.basename,
            )
            final = late_obj
        if src.basename in checks:
            check = checks.pop(src.basename)
            checked = ctx.actions.declare_file(out_dir + "/" + stem + ".checked.o")
            run_tool(
                ctx,
                ctx.attr.abi_check,
                [
                    "--class",
                    check.cls,
                    "--kernelcache",
                    ctx.file._kernelcache.path,
                    "--object",
                    obj.path,
                    "--late",
                    final.path,
                    "--bindings",
                    check.bindings.path,
                    "--output",
                    checked.path,
                ],
                depset([obj, final, check.bindings, ctx.file._kernelcache]),
                [checked],
                "KernelAbiCheck",
                "Checking {} against the original kernelcache ({})".format(src.basename, check.cls),
            )
            final = checked
        objects.append(final)
    if checks:
        fail("{}: abi_checks for sources not in srcs: {}".format(ctx.label, checks.keys()))
    return [
        DefaultInfo(files = depset(objects)),
        KernelObjectsInfo(objects = objects, prelink = ctx.attr.prelink, cuts = ctx.attr.prelink_cuts,
                          deferred = ctx.attr.prelink_deferred),
    ]

_kernel_driver = rule(
    implementation = _kernel_driver_impl,
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "srcs": attr.label_list(allow_files = [".cpp", ".cc", ".mm", ".c", ".S", ".s"], mandatory = True),
        "dir": attr.string(mandatory = True),
        "stems": attr.string_dict(doc = "Object names for sources whose stem would collide (basename -> stem)."),
        "object_names": attr.string(default = "stem", values = ["stem", "path"]),
        "object_prefix": attr.string(),
        "src_copts": attr.string_list_dict(doc = "Extra flags for one source (basename -> flags)."),
        "include_osfmk": attr.bool(default = True, doc = "C++: -I <xnu>/osfmk after our warnings (ioswitch_driver)."),
        "hdrs": attr.label_list(allow_files = True),
        "out_dir": attr.string(),
        "recipe": attr.label(default = "//out/native-a57/root-v1/tree:iokit_cxx", providers = [RecipeInfo]),
        "c_recipe": attr.label(default = "//out/native-a57/root-v1/tree:osfmk_c", providers = [RecipeInfo]),
        "c_command": attr.string_list(),
        "warnings": attr.bool(default = True),
        "copts": attr.string_list(),
        "c_copts": attr.string_list(),
        "asm_copts": attr.string_list(),
        "late": attr.string_list(),
        "abi_checks": attr.label_keyed_string_dict(allow_files = True),
        "abi_check": attr.label(executable = True, cfg = "exec", doc = "the ABI checker (a py_binary)"),
        "prelink": attr.string_list(),
        "prelink_cuts": attr.string_list(),
        "prelink_deferred": attr.string_list(),
        "xnu": attr.label(default = "//out/native-a57/root-v1/tree:xnu", providers = [XnuInfo]),
        "_xnu_src": attr.label(default = "@xnu//:all"),
        "_common_hdrs": attr.label(default = "//out/native-a57/root-v1/tree:kernel_headers"),
        "_objcopy": attr.label(default = "@llvm//:bin/llvm-objcopy", executable = True, allow_single_file = True, cfg = "exec"),
        "_kernelcache": attr.label(default = "//ipsw:kernelcache", allow_single_file = True),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def kernel_driver(name, srcs = None, dir = None, **kwargs):
    """A kernel driver: its objects, in link order.

    dir defaults to drivers/<name>; srcs to its .cpp files (sorted) followed by its firmware.S, if it has one. Names in
    srcs are relative to dir; anything else is a label ("//bazel/pinned:...", "@xnu//:...")."""
    dir = dir or ("drivers/" + name)
    if srcs == None:
        srcs = sorted(native.glob([dir + "/*.cpp"])) + native.glob([dir + "/firmware.S"], allow_empty = True)
    else:
        srcs = [s if (s.startswith(":") or s.startswith("/") or s.startswith("@")) else dir + "/" + s for s in srcs]
    _kernel_driver(name = name, srcs = srcs, dir = native.package_name() + "/" + dir, **kwargs)

def _kernel_archive_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    members = []
    for m in ctx.attr.members:
        if KernelObjectsInfo in m:
            members += m[KernelObjectsInfo].objects
        else:
            members += m.files.to_list()
    args = ctx.actions.args()
    args.add_all(["--xcode-env", ctx.file._xcode_env.path, "--out", out.path])
    args.add_all(members)
    run_tool(
        ctx,
        "archive.py",
        ["--xcode-env", ctx.file._xcode_env.path, "--out", out.path] + [m.path for m in members],
        depset(members),
        [out],
        "KernelArchive",
        "Archiving " + ctx.attr.out,
    )
    prelink, cuts, deferred = [], [], []
    for m in ctx.attr.members:
        if KernelObjectsInfo in m:
            prelink += m[KernelObjectsInfo].prelink
            cuts += m[KernelObjectsInfo].cuts
            deferred += m[KernelObjectsInfo].deferred
    return [
        DefaultInfo(files = depset([out])),
        KernelObjectsInfo(objects = [out], prelink = prelink, cuts = cuts, deferred = deferred),
    ]

kernel_archive = rule(
    implementation = _kernel_archive_impl,
    doc = "A static archive of drivers' objects, members in the given order (with -force_load, the link order).",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(mandatory = True),
        "members": attr.label_list(allow_files = [".o"]),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _stage_files_impl(ctx):
    outs = []
    for target, path in ctx.attr.files.items():
        src = target.files.to_list()[0]
        out = ctx.actions.declare_file(path)
        ctx.actions.symlink(output = out, target_file = src)
        outs.append(out)
    return [DefaultInfo(files = depset(outs))]

stage_files = rule(
    implementation = _stage_files_impl,
    doc = "Places files at paths below this package's output directory (e.g. pinned headers at build/<dir>/, where " +
          "the CMake flow's -I flags expected them).",
    attrs = {"files": attr.label_keyed_string_dict(allow_files = True)},
)

def _kernel_link_impl(ctx):
    xnu = ctx.attr.xnu[XnuInfo]
    out = ctx.actions.declare_file("build/xnu/mach.development.bcm2837.unstripped.noctf")
    if len(ctx.attr.inputs) != len(ctx.attr.modes):
        fail("inputs and modes must pair up")
    specs, files = [], []
    for target, mode in zip(ctx.attr.inputs, ctx.attr.modes):
        if KernelObjectsInfo in target:
            items = target[KernelObjectsInfo].objects
        else:
            items = target.files.to_list()
            archives = [f for f in items if f.extension == "a"]
            if len(items) > 1 and len(archives) == 1:
                items = archives
        if len(items) != 1:
            fail("{}: link input {} must be one file (or have one .a)".format(ctx.label, target.label))
        specs.append(mode + ":" + items[0].path)
        files += items
    specs = ["lib-dir:" + xnu.sdkroot.path + "/usr/local/lib/kernel", "lib:firehose_kernel"] + specs
    run_tool(
        ctx,
        "link.py",
        [
            "--xcode-env",
            ctx.file._xcode_env.path,
            "--link-json",
            xnu.link.path,
            "--objroot",
            xnu.objroot.path,
            "--sdkroot",
            xnu.sdkroot.path,
            "--output",
            out.path,
            "--bin-dir",
            ctx.bin_dir.path,
        ] + specs,
        depset([xnu.objroot, xnu.link, xnu.sdkroot] + files),
        [out],
        "KernelLink",
        "Linking XNU with the ioswitch archives",
    )
    prelink, cuts, deferred = [], [], []
    for target in ctx.attr.inputs:
        if KernelObjectsInfo in target:
            prelink += target[KernelObjectsInfo].prelink
            cuts += target[KernelObjectsInfo].cuts
            deferred += target[KernelObjectsInfo].deferred
    return [
        DefaultInfo(files = depset([out])),
        KernelObjectsInfo(objects = [out], prelink = prelink, cuts = cuts, deferred = deferred),
    ]

kernel_link = rule(
    implementation = _kernel_link_impl,
    doc = "XNU's recorded kernel link with LD_KERNEL_ARCHIVES = inputs (modes: force_load or plain), in order.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "xnu": attr.label(mandatory = True, providers = [XnuInfo]),
        "inputs": attr.label_list(allow_files = True),
        "modes": attr.string_list(),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _kernel_prelink_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    report = ctx.actions.declare_file(ctx.attr.out[:-len(".macho")] + ".json")
    kernel = ctx.attr.kernel[KernelObjectsInfo]
    new_roots = sorted({r: True for r in kernel.prelink}.keys())
    args = [
        "--kernel",
        kernel.objects[0].path,
        "--prelinked",
        ctx.file.kernelcache.path,
        "--inherit-original-report",
        ctx.file.inherit_report.path,
        "--previous",
        ctx.file.previous.path,
        "--slide=" + ctx.attr.slide,
        "--output",
        out.path,
    ]
    for root in ctx.attr.base_roots + new_roots:
        args += ["--root", root]
    for root in new_roots:
        args += ["--new-root", root]
    for cut in kernel.cuts:
        args += ["--cut", cut]
    for defer in kernel.deferred:
        args += ["--defer", defer]
    for init in ctx.attr.early_security_init:
        args += ["--early-security-init", init]
    run_tool(
        ctx,
        ctx.attr._emit_prelink,
        args,
        depset([kernel.objects[0], ctx.file.kernelcache, ctx.file.inherit_report, ctx.file.previous]),
        [out, report],
        "KernelPrelink",
        "Prelinking the original kexts into " + ctx.attr.out,
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(report = depset([report]))]

kernel_prelink = rule(
    implementation = _kernel_prelink_impl,
    doc = "tools/prelink/emit_prelink.py: Apple's original kexts (the V20 roots plus every driver's prelink list) " +
          "prelinked into the linked kernel.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "out": attr.string(default = "kernel.macho"),
        "kernel": attr.label(mandatory = True, providers = [KernelObjectsInfo]),
        "kernelcache": attr.label(default = "//ipsw:kernelcache", allow_single_file = True),
        "inherit_report": attr.label(mandatory = True, allow_single_file = True),
        "previous": attr.label(mandatory = True, allow_single_file = True),
        "base_roots": attr.string_list(mandatory = True),
        "slide": attr.string(mandatory = True),
        "early_security_init": attr.string_list(),
        "_emit_prelink": attr.label(default = "//tools/prelink:emit_prelink", executable = True, cfg = "exec"),
    }),
    toolchains = [PY_TOOLCHAIN],
)
