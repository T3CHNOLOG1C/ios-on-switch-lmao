"""Code linked into the kernel: drivers (cc_library targets on the kernel toolchain), archives, the link, the prelink.

Adding a driver is one call in out/native-a57/root-v1/tree/BUILD.bazel:

    kernel_driver(name = "SwitchFoo")                     # drivers/SwitchFoo/*.cpp, IOKit C++ recipe, our warnings

plus its name in the member list of the archive it rides in (archive member order is link order: -force_load).
Knobs: srcs (ordered; .cpp/.c/.S, defaults to the directory's .cpp files), late (static initializer run by the board
code after the original family loads), abi_checks (SwitchUSB-style checks against the original kernelcache),
firmware (a firmware.S that .incbins the directory's firmware/), copts / c_copts / asm_copts, recipe / c_recipe /
c_command (the compile recipe: a feature of the kernel toolchain, bazel/toolchains/kernel), hdrs, hdr_deps (the header
directories the driver includes besides its own), prelink (original kexts this driver needs prelinked: they join the
prelink roots).

A driver is cc_library targets for the kernel platform (compiled with the kernel toolchain; .d files prune the headers
each object really includes) plus <name>, the driver's objects in link order under the names the archives always had:
  <name>_cc            its own C/C++ sources (clang-tidy's)
  <name>_foreign_cc    vendored sources (linux/, nouveau/, XNU's), tagged no-lint
  <name>_<stem>_cc     a source with its own flags (src_copts)
  <name>_asm_cc        assembly (bindings, firmware .incbin)
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@xcode//:defs.bzl", "IPHONEOS_SDK", "XCODE_TOOLCHAIN")
load("//bazel/platforms:transitions.bzl", "kernel_transition")
load(":common.bzl", "KERNEL_TOOL_ATTRS", "PY_TOOLCHAIN", "XCODE_EXECUTION", "run_tool")
load(":xnu.bzl", "XnuInfo")

KernelObjectsInfo = provider(
    doc = "Objects in link order, and the original kexts they need prelinked.",
    fields = ["objects", "prelink", "cuts", "deferred"],
)


# Compile recipes (the old recipe/c_recipe labels' names) -> the kernel toolchain's features.
_CXX_RECIPES = ["iokit_cxx", "iokit_cxx_generic"]
_C_RECIPES = ["osfmk_c", "bsd_c", "mockfs_c", "freestanding_c"]

# Vendored code below a driver's directory: its own cc_library, which clang-tidy skips.
FOREIGN_DIRS = ["linux/", "nouveau/"]

# Generated headers a driver's flags name ({build}/<dir>) -> the target in the tree package that brings them.
GENERATED_HEADERS = {
    "storage-abi": ":gen_storage_abi",
    "native-ram-block": ":gen_native_data_seed",
    "native-legacy-display": ":gen_pinned_headers",
    "native-hid-input": ":gen_pinned_headers",
    "sheng-platform-power": ":gen_pinned_headers",
}

_HEADER_GLOBS = ["**/*.h", "**/*.hpp", "**/*.inc"]
_KERNEL_ONLY = ["//bazel/platforms:kernel"]

def _recipe_name(label, valid):
    name = label.rpartition(":")[2]
    if name not in valid:
        fail("unknown compile recipe {} (one of {})".format(label, valid))
    return name

def _expand(flags, package):
    """{tree}, {build}, {xnu}, {sdk} in a driver's flags: make variables of the kernel SDK (//...:kernel_sdk) or paths."""
    return [
        f.replace("{tree}", package)
            .replace("{build}", "$(KERNEL_BUILD)")
            .replace("{xnu}", "$(KERNEL_XNU)")
            .replace("{sdk}", IPHONEOS_SDK)
        for f in flags
    ]

def _generated(flags):
    deps = []
    for f in flags:
        if "{build}/" in f:
            directory = f.partition("{build}/")[2].partition("/")[0]
            dep = GENERATED_HEADERS.get(directory)
            if dep and dep not in deps:
                deps.append(dep)
    return deps

def _src_path(src):
    """The package-relative path (or label) of a source, and its basename."""
    path = src[1:] if src.startswith(":") else src
    return path, path.rpartition("/")[2].rpartition(":")[2]

def kernel_header_libraries(dirs, extra = []):
    """hdrs_<dir> for each directory (its headers, any depth) and all_kernel_headers (every one, plus extra)."""
    names = []
    for d in dirs:
        name = "hdrs_" + d.replace("/", "_")
        cc_library(
            name = name,
            hdrs = native.glob([d + "/" + g for g in _HEADER_GLOBS], allow_empty = True),
            target_compatible_with = _KERNEL_ONLY,
            tags = ["kernel"],
        )
        names.append(":" + name)
    cc_library(
        name = "hdrs_extra",
        hdrs = extra,
        target_compatible_with = _KERNEL_ONLY,
        tags = ["kernel"],
    )
    cc_library(
        name = "all_kernel_headers",
        deps = names + [":hdrs_extra"],
        target_compatible_with = _KERNEL_ONLY,
        tags = ["kernel"],
    )

def kernel_driver(
        name,
        srcs = None,
        dir = None,
        stems = {},
        object_names = "stem",
        object_prefix = "",
        src_copts = {},
        include_osfmk = True,
        hdrs = [],
        hdr_deps = None,
        out_dir = None,
        recipe = None,
        c_recipe = None,
        c_command = None,
        warnings = True,
        copts = [],
        c_copts = [],
        asm_copts = [],
        late = [],
        abi_checks = {},
        abi_check = None,
        prelink = [],
        prelink_cuts = [],
        prelink_deferred = [],
        xnu = None,  # @unused: the kernel SDK replaced the XNU build as a compile input
        visibility = None,
        tags = []):
    """A kernel driver: cc_library targets for the kernel platform, and <name>, its objects in link order.

    dir defaults to drivers/<name>; srcs to its .cpp files (sorted) followed by its firmware.S, if it has one. Names in
    srcs are relative to dir; anything else is a label ("//bazel/pinned:...", "@xnu//:..."). hdr_deps: header
    directories (e.g. "drivers/ArmGIC") the driver includes besides its own; None means every one."""
    package = native.package_name()
    dir = dir or ("drivers/" + name)
    if srcs == None:
        srcs = sorted(native.glob([dir + "/*.cpp"])) + native.glob([dir + "/firmware.S"], allow_empty = True)
    else:
        srcs = [s if (s.startswith(":") or s.startswith("/") or s.startswith("@")) else dir + "/" + s for s in srcs]

    features = []
    if recipe:
        features += ["-kernel_iokit_cxx", "kernel_" + _recipe_name(recipe, _CXX_RECIPES)]
    if c_command:
        features += ["-kernel_osfmk_c", "kernel_plain_c"]
    elif c_recipe:
        features += ["-kernel_osfmk_c", "kernel_" + _recipe_name(c_recipe, _C_RECIPES)]
    if not warnings:
        features.append("-kernel_warnings")

    cxx_flags = (["-I", "{xnu}/osfmk"] if include_osfmk else []) + copts
    if hdr_deps == None:
        header_deps = [":kernel_sdk", ":all_kernel_headers"]
    else:
        # Its own directory's headers (or, below drivers/X, X's), the ones it names, and the extra ones (hdrs_extra).
        own = "/".join(dir.split("/")[:2]) if dir.startswith("drivers/") or dir.startswith("kits/") else dir
        header_deps = [":kernel_sdk"]
        if native.existing_rule("hdrs_" + own.replace("/", "_")):
            header_deps.append(":hdrs_" + own.replace("/", "_"))
        for d in hdr_deps:
            lib = "hdrs_" + d.replace("/", "_")
            if d == own:
                continue
            if not native.existing_rule(lib):
                fail("{}: hdr_deps names {}, which has no headers (kernel_header_libraries)".format(name, d))
            header_deps.append(":" + lib)
        header_deps.append(":hdrs_extra")

    # Sources by the cc_library that compiles them; parts[i] is the index of source i's library.
    groups, order, parts = {}, [], []
    for src in srcs:
        path, base = _src_path(src)
        ext = base.rpartition(".")[2]
        if ext in ("S", "s"):
            key = "asm"
        elif base in src_copts:
            key = base
        elif src.startswith("@") or any([path.startswith(dir + "/" + f) for f in FOREIGN_DIRS]):
            key = "foreign"
        elif ext in ("cpp", "cc", "mm", "c"):
            key = "own"
        else:
            fail("{}: unsupported source {}".format(name, src))
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(src)
        parts.append(order.index(key))

    cc_deps = []
    for key in order:
        group = groups[key]
        extra = src_copts.get(key, [])
        if key == "asm":
            lib = name + "_asm_cc"
            firmware = [s for s in group if _src_path(s)[1] == "firmware.S"]
            asm_flags = list(asm_copts)
            additional = []
            if firmware:
                fw_dir = _src_path(firmware[0])[0].rpartition("/")[0] + "/firmware"
                asm_flags += ["-I", "{tree}/" + fw_dir]
                additional = native.glob([fw_dir + "/**"], allow_empty = True)
            cc_library(
                name = lib,
                srcs = group,
                copts = _expand(asm_flags, package),
                additional_compiler_inputs = additional,
                deps = header_deps,
                toolchains = [":kernel_sdk"],
                target_compatible_with = _KERNEL_ONLY,
                tags = ["kernel", "no-lint"] + tags,
            )
        else:
            lib = name + ("_foreign_cc" if key == "foreign" else ("_cc" if key == "own" else "_" + key.rpartition(".")[0] + "_cc"))
            flags = cxx_flags + c_copts + extra
            cc_library(
                name = lib,
                srcs = group,
                hdrs = hdrs,
                cxxopts = _expand(cxx_flags + extra, package),
                conlyopts = _expand(c_copts + extra, package),
                features = features,
                deps = header_deps + _generated(flags),
                toolchains = [":kernel_sdk"],
                target_compatible_with = _KERNEL_ONLY,
                tags = ["kernel"] + (["no-lint"] if key == "foreign" else []) + tags,
            )
        cc_deps.append(":" + lib)

    _kernel_driver_objects(
        name = name,
        srcs = srcs,
        parts = parts,
        cc_deps = cc_deps,
        dir = package + "/" + dir,
        stems = stems,
        object_names = object_names,
        object_prefix = object_prefix,
        out_dir = out_dir or "",
        late = late,
        abi_checks = abi_checks,
        abi_check = abi_check,
        prelink = prelink,
        prelink_cuts = prelink_cuts,
        prelink_deferred = prelink_deferred,
        visibility = visibility,
        tags = tags,
    )

def _stem(f):
    return f.basename[:f.basename.rfind(".")]

def _library_objects(target):
    """A cc_library's objects, in its srcs order."""
    objects = []
    for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
        if linker_input.owner == target.label:
            for library in linker_input.libraries:
                objects += library.objects
    return objects

def _kernel_driver_objects_impl(ctx):
    out_dir = ctx.attr.out_dir or ("build/" + ctx.label.name)
    late = {name: True for name in ctx.attr.late}
    checks = {}
    for bindings, spec in ctx.attr.abi_checks.items():
        cls, source = spec.split(" ")
        checks[source] = struct(cls = cls, bindings = bindings.files.to_list()[0])
    compiled = [_library_objects(dep) for dep in ctx.attr.cc_deps]
    taken = [0 for _ in compiled]
    if len(ctx.files.srcs) != len(ctx.attr.parts):
        fail("{}: srcs and parts differ".format(ctx.label))

    # rules_cc names a library's objects _objs/<library>/<stem>.o, or _objs/<library>/<n>/<stem>.o for the n-th of
    # several sources with one stem (case-insensitive, in srcs order): what each source's object must be.
    totals = [{} for _ in compiled]
    for src, part in zip(ctx.files.srcs, ctx.attr.parts):
        totals[part][_stem(src).lower()] = totals[part].get(_stem(src).lower(), 0) + 1
    seen = [{} for _ in compiled]
    objects = []
    for src, part in zip(ctx.files.srcs, ctx.attr.parts):
        if taken[part] >= len(compiled[part]):
            fail("{}: {} has no object in {}".format(ctx.label, src.short_path, ctx.attr.cc_deps[part].label))
        compiled_obj = compiled[part][taken[part]]
        taken[part] += 1
        key = _stem(src).lower()
        expected = "_objs/{}/".format(ctx.attr.cc_deps[part].label.name)
        if totals[part][key] > 1:
            expected += "{}/".format(seen[part].get(key, 0))
            seen[part][key] = seen[part].get(key, 0) + 1
        expected += _stem(src) + ".o"
        if not compiled_obj.path.endswith("/" + expected):
            fail("{}: object {} does not belong to {} (expected .../{})".format(
                ctx.label, compiled_obj.path, src.short_path, expected))
        ext = src.extension
        stem = _stem(src)
        if ext == "c" and ctx.attr.object_names == "path":
            # cmake/Drivers.cmake names vendored C objects by their path below the driver (nouveau has many base.c).
            stem = ctx.attr.object_prefix + src.short_path[len(ctx.attr.dir) + 1:].replace("/", "_")
        stem = ctx.attr.stems.get(src.basename, stem)
        if not stem:
            fail("empty stem for {} (dir {}, short_path {})".format(src, ctx.attr.dir, src.short_path))
        # The object under the name and path the archives and the link always used.
        obj = ctx.actions.declare_file(out_dir + "/" + stem + ".o")
        ctx.actions.symlink(output = obj, target_file = compiled_obj)
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
    for part, objs in enumerate(compiled):
        if taken[part] != len(objs):
            fail("{}: {} has objects no source claims".format(ctx.label, ctx.attr.cc_deps[part].label))
    if checks:
        fail("{}: abi_checks for sources not in srcs: {}".format(ctx.label, checks.keys()))
    return [
        DefaultInfo(files = depset(objects)),
        KernelObjectsInfo(objects = objects, prelink = ctx.attr.prelink, cuts = ctx.attr.prelink_cuts,
                          deferred = ctx.attr.prelink_deferred),
    ]

_kernel_driver_objects = rule(
    implementation = _kernel_driver_objects_impl,
    doc = "A driver's objects (from its kernel cc_library targets) in source order, named as the archives expect; " +
          "late objects and ABI checks.",
    attrs = dict(KERNEL_TOOL_ATTRS, **{
        "srcs": attr.label_list(allow_files = [".cpp", ".cc", ".mm", ".c", ".S", ".s"], mandatory = True),
        "parts": attr.int_list(mandatory = True, doc = "for each source, the index of the cc_deps entry compiling it"),
        "cc_deps": attr.label_list(providers = [CcInfo], cfg = kernel_transition, mandatory = True),
        "dir": attr.string(mandatory = True),
        "stems": attr.string_dict(doc = "Object names for sources whose stem would collide (basename -> stem)."),
        "object_names": attr.string(default = "stem", values = ["stem", "path"]),
        "object_prefix": attr.string(),
        "out_dir": attr.string(),
        "late": attr.string_list(),
        "abi_checks": attr.label_keyed_string_dict(allow_files = True),
        "abi_check": attr.label(executable = True, cfg = "exec", doc = "the ABI checker (a py_binary)"),
        "prelink": attr.string_list(),
        "prelink_cuts": attr.string_list(),
        "prelink_deferred": attr.string_list(),
        "_objcopy": attr.label(default = "@llvm//:bin/llvm-objcopy", executable = True, allow_single_file = True, cfg = "exec"),
        "_kernelcache": attr.label(default = "//ipsw:kernelcache", allow_single_file = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    }),
    toolchains = [PY_TOOLCHAIN],
)

def _kernel_archive_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    members = []
    for m in ctx.attr.members:
        if KernelObjectsInfo in m:
            members += m[KernelObjectsInfo].objects
        else:
            members += m.files.to_list()
    args = ctx.actions.args()
    args.add("rcs")
    args.add(out)
    args.add_all(members)
    # Xcode's ar (cmake/XnuRecipes.cmake ioswitch_archive: `xcrun ar rcs`); ZERO_AR_DATE=1 zeroes the member dates.
    ctx.actions.run(
        executable = XCODE_TOOLCHAIN + "/ar",
        arguments = [args],
        inputs = depset(members + [ctx.file._xcode_env, ctx.file._xcode_identity]),
        outputs = [out],
        env = {
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
            "DEVELOPER_DIR": XCODE_TOOLCHAIN.rpartition("/Toolchains/")[0],
            "LC_ALL": "C",
            "LANG": "C",
            "TZ": "UTC",
            "ZERO_AR_DATE": "1",
        },
        mnemonic = "KernelArchive",
        progress_message = "Archiving " + ctx.attr.out,
        execution_requirements = XCODE_EXECUTION,
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
