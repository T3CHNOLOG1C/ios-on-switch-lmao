"""Rules for the TegraGM20B Metal bundle (gpu/BUILD.bazel). Each step of the old build-tegra-gm20b-metal.py is one
action with declared inputs, so Bazel reruns only what an edit reaches:

  llvm-as (2 fixtures) -> rustc per crate (iOS bridge: bazel/rust) -> clang++ (each object) -> link -> strip/sign/trust cache
                       -> rustc per crate (host bridge) -> precompile tool -> seed (runs on this Mac)
"""

load("//bazel/ipsw:ipsw.bzl", "IpswInfo")
load("//bazel/kernel:layout.bzl", "MOVED_ROOTS", "OLD_TREE", "TREE_ROOT")

_XCODE = {
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
}

def _repo_root(files):
    """The execroot path of the external repository the files come from."""
    return files[0].owner.workspace_root

# --- llvm-as ---------------------------------------------------------------------------------------------------------

def _llvm_as_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.run(
        executable = ctx.executable._llvm_as,
        arguments = ["-o", out.path, ctx.file.src.path],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "LlvmAs",
        progress_message = "Assembling %{input}",
    )
    return [DefaultInfo(files = depset([out]))]

llvm_as = rule(
    implementation = _llvm_as_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = [".ll"]),
        "out": attr.string(mandatory = True),
        "_llvm_as": attr.label(default = "@gpu_llvm_as//:bin/llvm-as", executable = True, cfg = "exec", allow_single_file = True),
    },
)

def _all_cores(_os, _inputs):
    return {"cpu": 8, "memory": 4096}

# --- Xcode clang: objects and the plugin link --------------------------------------------------------------------

def _xcode_run(ctx, mnemonic, message, command, inputs, outputs, env = {}):
    args = ctx.actions.args()
    args.add(ctx.file._xcode_env)
    args.add_all(["%s=%s" % (k, v) for k, v in env.items()])
    args.add("--")
    args.add_all(command)
    ctx.actions.run(
        executable = ctx.executable._xcode_run,
        arguments = [args],
        inputs = depset(inputs + [ctx.file._xcode_env, ctx.file._xcode_identity]),
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = message,
    )

_IOS = ["-target", "arm64-apple-ios17.0", "-isysroot", "@IPHONEOS_SDK@"]

# The old script's one clang++ line, split into compile and link so each object is its own cached action. Paths in
# debug info and __FILE__ are mapped to /ioswitch, and the link keeps no object paths (oso_prefix) or times.
# -fno-objc-msgsend-selector-stubs: ld's own objc_msgSend selector stubs (its default "fast" __objc_stubs, which the
# old build had) come out in a different order in about half of all links (measured: 2 outputs in 10 links), so the
# plugin's CDHash, and with it the trust cache and the kernel window, changed on every build. Classic call sites
# (selector loaded inline, objc_msgSend through __stubs) take the same instructions per message and link identically
# (10 of 10), at +10 KB of __text.
_PLUGIN_CFLAGS = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fstack-protector-strong", "-std=c++17", "-fobjc-arc",
                  "-Wno-deprecated-declarations", "-fno-objc-msgsend-selector-stubs", "-ffile-prefix-map=@PWD@=/ioswitch"] + [
    # The layout move's old spellings (bazel/kernel/layout.bzl): the plugin's strings name its sources as before.
    "-ffile-prefix-map=@PWD@/{r}=/ioswitch/{o}/{r}".format(r = r, o = OLD_TREE)
    for r in MOVED_ROOTS
    if TREE_ROOT != OLD_TREE
] + [
    # The sources are compiled by their execution-root-relative path (src.path), which the line tables' directories
    # and __FILE__ keep relative: those get the old spelling too.
    "-ffile-prefix-map={r}={o}/{r}".format(r = r, o = OLD_TREE)
    for r in MOVED_ROOTS
    if TREE_ROOT != OLD_TREE
]

def _metal_plugin_impl(ctx):
    # -I in the old build's order (NativeMetal, raw/runtime, TegraGM20B), and the raw path's stack key as a define.
    includes = []
    for marker in ctx.files.include_markers:
        includes += ["-I", marker.dirname]
    defines = ["-DTEGRA_RAW_STACK=0x@FILE:%s@ull" % ctx.file.stack.path] if ctx.file.stack else []
    inputs = ctx.files.srcs + ctx.files.cc_srcs + ctx.files.hdrs + ctx.files.include_markers + ctx.files.stack
    objects = []
    for language, sources in (("objective-c++", ctx.files.srcs), ("c++", ctx.files.cc_srcs)):
        # Plain C++ sends no Objective-C messages (clang calls the flag unused there, an error under -Werror).
        cflags = [f for f in _PLUGIN_CFLAGS if language == "objective-c++" or f != "-fno-objc-msgsend-selector-stubs"]
        for src in sources:
            obj = ctx.actions.declare_file("%s_objs/%s.o" % (ctx.attr.name, src.basename))
            _xcode_run(
                ctx,
                "ObjcxxCompile",
                "Compiling %s" % src.short_path,
                ["clang++"] + _IOS + cflags + includes + defines + ["-x", language, "-c", src.path, "-o", obj.path],
                inputs,
                [obj],
            )
            objects.append(obj)
    out = ctx.actions.declare_file(ctx.attr.name + "/TegraGM20BMetal")
    frameworks = []
    for framework in ctx.attr.frameworks:
        frameworks += ["-framework", framework]
    _xcode_run(
        ctx,
        "MetalPluginLink",
        "Linking the TegraGM20BMetal plugin",
        # The compile flags again, as the old one-line build had them: clang passes -O2 on to ld, whose code
        # deduplication it turns on (118 KB less __text).
        ["clang++"] + _IOS + _PLUGIN_CFLAGS + ["-bundle"] + [o.path for o in objects] + [ctx.file.bridge.path] +
        ["-Wl,-dead_strip", "-Wl,-oso_prefix,@PWD@/", "-lobjc"] + frameworks + ["-o", out.path],
        objects + [ctx.file.bridge],
        [out],
    )
    dsym = ctx.actions.declare_directory(ctx.attr.name + "/TegraGM20BMetal.dSYM")
    _xcode_run(
        ctx,
        "Dsymutil",
        "Writing the plugin's dSYM",
        ["dsymutil", "--oso-prepend-path=@PWD@", out.path, "-o", dsym.path],
        objects + [ctx.file.bridge, out],
        [dsym],
    )
    return [
        DefaultInfo(files = depset([out])),
        OutputGroupInfo(debug = depset([dsym])),
    ]

metal_plugin = rule(
    implementation = _metal_plugin_impl,
    attrs = _XCODE | {
        "srcs": attr.label_list(mandatory = True, allow_files = [".mm"], doc = "Objective-C++ units, in link order"),
        "cc_srcs": attr.label_list(allow_files = [".cpp"], doc = "C++ units, linked after srcs"),
        "hdrs": attr.label_list(allow_files = True),
        "include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory, in order"),
        "stack": attr.label(allow_single_file = True, doc = "the raw path's stack key (TEGRA_RAW_STACK)"),
        "frameworks": attr.string_list(default = ["Foundation", "Metal", "IOSurface"]),
        "bridge": attr.label(mandatory = True, allow_single_file = True),
        "_xcode_run": attr.label(default = "//tools/gpu:xcode_run", executable = True, cfg = "exec"),
    },
)

# --- The raw GM20B path: its stack key and its pipeline pack -------------------------------------------------------

def _raw_stack_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".txt")
    bridge = [f for f in ctx.files.srcs if f.basename == "NativeMetalBridge.rs"]
    if len(bridge) != 1:
        fail("srcs must name NativeMetal's bridge/NativeMetalBridge.rs once")
    drivers = bridge[0].dirname.rsplit("/NativeMetal/", 1)[0]
    m2v_root = ctx.attr.m2v_root.label.package
    args = ctx.actions.args()
    args.add("--nvk", ctx.file.nvk)
    args.add("--drivers", drivers)
    args.add("--m2v-root", m2v_root)
    args.add_all(ctx.files.m2v, before_each = "--m2v")
    args.add("--raw", drivers + "/TegraGM20BMetal/raw")
    args.add("--out", out)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.nvk] + ctx.files.m2v + ctx.files.srcs,
        outputs = [out],
        mnemonic = "RawStack",
        progress_message = "Hashing the raw path's stack key",
    )
    return [DefaultInfo(files = depset([out]))]

raw_stack = rule(
    implementation = _raw_stack_impl,
    attrs = {
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "m2v": attr.label_list(allow_files = True, doc = "the source files of the m2v crates the bridge links"),
        "m2v_root": attr.label(mandatory = True, allow_single_file = True, doc = "the m2v tree's package (its files are hashed by their path below it)"),
        "srcs": attr.label_list(allow_files = True, doc = "the bridge sources, the frontend, every raw/ source"),
        "_tool": attr.label(default = "//tools/gpu:raw_stack", executable = True, cfg = "exec"),
    },
)

def _metal_raw_pack_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.name)
    tree = ctx.file.generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    twin = _twin(ctx, ctx.executable._seed_tool)
    args = ctx.actions.args()
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--host-bridge", ctx.file.host_bridge)
    args.add("--twin-dylib", twin)
    args.add("--qc", ctx.file.qc)
    args.add("--corpus", ctx.file.corpus)
    args.add("--stack", ctx.file.stack)
    args.add("--generator", ctx.file.generator)
    args.add("--tree", tree)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset(
            [ctx.file.host_bridge, twin, ctx.file.qc, ctx.file.corpus, ctx.file.stack, ctx.file.generator,
             ctx.file._xcode_env, ctx.file._xcode_identity] + ctx.files.srcs,
        ),
        outputs = [out],
        mnemonic = "MetalRawPack",
        progress_message = "Making the raw GM20B path's pipeline pack",
        # The probe runs on this Mac (Metal + the NVK twin over the mock GM20B): never on a remote executor.
        execution_requirements = {"no-remote-exec": "1"},
        resource_set = _all_cores,
    )
    return [DefaultInfo(files = depset([out]))]

metal_raw_pack = rule(
    implementation = _metal_raw_pack_impl,
    attrs = _XCODE | {
        "host_bridge": attr.label(mandatory = True, allow_single_file = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "twin": attr.label(mandatory = True, allow_single_file = True),
        "nvk_sources": attr.label_list(allow_files = True),
        "qc": attr.label(mandatory = True, allow_single_file = True),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "stack": attr.label(mandatory = True, allow_single_file = True),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the pack generator's sources, the frontend, the ABI headers"),
        "_tool": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
        "_seed_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
    },
)

# --- The signed bundle and its trust cache -----------------------------------------------------------------------

def _metal_bundle_impl(ctx):
    bundle = ctx.actions.declare_directory("TegraGM20BMetal.bundle")
    trustcache = ctx.actions.declare_file("TegraGM20BMetal.trustcache")
    trust_json = ctx.actions.declare_file("trust.json")
    args = ctx.actions.args()
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--plugin", ctx.file.plugin)
    args.add("--nvk", ctx.file.nvk)
    args.add("--bundle", bundle.path)
    args.add("--trustcache", trustcache)
    args.add("--trust-json", trust_json)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.plugin, ctx.file.nvk, ctx.file._xcode_env, ctx.file._xcode_identity],
        outputs = [bundle, trustcache, trust_json],
        mnemonic = "MetalBundle",
        progress_message = "Signing TegraGM20BMetal.bundle and writing its trust cache",
    )
    return [
        DefaultInfo(files = depset([bundle, trustcache, trust_json])),
        OutputGroupInfo(bundle = depset([bundle]), trustcache = depset([trustcache]), trust_json = depset([trust_json])),
    ]

metal_bundle = rule(
    implementation = _metal_bundle_impl,
    attrs = _XCODE | {
        "plugin": attr.label(mandatory = True, allow_single_file = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "_tool": attr.label(default = "//tools/gpu:bundle", executable = True, cfg = "exec"),
    },
)

# --- The shader cache seed ---------------------------------------------------------------------------------------

def _m2v_library_names_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".json")
    root = _repo_root(ctx.files.libraries) + "/root/"
    args = ctx.actions.args()
    args.add("names")
    args.add(root)
    args.add(out)
    devices = [f.path[len(root):] for f in ctx.files.libraries if f.path.startswith(root)]
    if len(devices) != len(ctx.files.libraries):
        fail("every library must be below " + root)
    args.add_all(devices)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = ctx.files.libraries,
        outputs = [out],
        mnemonic = "M2vLibraryNames",
        progress_message = "Indexing the function names of %d Metal libraries" % len(ctx.files.libraries),
    )
    return [DefaultInfo(files = depset([out]))]

m2v_library_names = rule(
    implementation = _m2v_library_names_impl,
    doc = "names.json (precompile/libraries.py names): every function name of the libraries -> their device paths.",
    attrs = {
        "libraries": attr.label(mandatory = True, doc = "@gpu_apple_libs//:libraries (root/<device path>)"),
        "_tool": attr.label(
            default = "//drivers/TegraGM20BMetal:libraries",
            executable = True,
            cfg = "exec",
        ),
    },
)

def _twin(ctx, tool):
    """The NVK host twin with the iOS dylib's pipeline cache identity (seed.py twin), as the seed and the raw pack run it."""
    out = ctx.actions.declare_file(ctx.attr.name + "_twin/libvulkan_nouveau.dylib")
    args = ctx.actions.args()
    args.add("twin")
    args.add("--nvk", ctx.file.nvk)
    args.add("--twin", ctx.file.twin)
    args.add("--out", out)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        inputs = [ctx.file.nvk, ctx.file.twin] + ctx.files.nvk_sources,
        outputs = [out],
        mnemonic = "NvkTwin",
        progress_message = "Giving the NVK host twin the iOS dylib's identity",
        execution_requirements = {"no-remote-exec": "1", "requires-darwin": ""},
    )
    return out

def _metal_seed_impl(ctx):
    xcode = [ctx.file._xcode_env, ctx.file._xcode_identity]
    seed_tool = ctx.executable._tool
    darwin = {"no-remote-exec": "1", "requires-darwin": ""}

    mock = ctx.actions.declare_file(ctx.attr.name + "_tool/libmock_iokit.dylib")
    args = ctx.actions.args()
    args.add("mock")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--abi-header", ctx.file.abi_header)
    args.add("--mock-iokit", ctx.file.mock_iokit)
    args.add("--out", mock)
    ctx.actions.run(
        executable = seed_tool,
        arguments = [args],
        inputs = [ctx.file.abi_header, ctx.file.mock_iokit] + xcode,
        outputs = [mock],
        mnemonic = "MetalSeedMock",
        progress_message = "Building the precompile's mock IOKit",
        execution_requirements = darwin,
    )

    tool = ctx.actions.declare_file(ctx.attr.name + "_tool/precompile")
    args = ctx.actions.args()
    args.add("tool")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--metal-header", ctx.file.metal_header)
    args.add("--precompile", ctx.file.precompile)
    args.add("--frontend", ctx.file.frontend)
    args.add("--host-bridge", ctx.file.host_bridge)
    args.add("--mock", mock)
    args.add("--out", tool)
    ctx.actions.run(
        executable = seed_tool,
        arguments = [args],
        inputs = [ctx.file.metal_header, ctx.file.precompile, ctx.file.frontend, ctx.file.host_bridge, mock] +
                 ctx.files.hdrs + xcode,
        outputs = [tool],
        mnemonic = "MetalSeedTool",
        progress_message = "Building the shader cache precompile tool",
        execution_requirements = darwin,
    )

    twin = _twin(ctx, seed_tool)

    # The corpus with the libraries of older journal lines resolved (precompile/libraries.py resolve).
    corpus = ctx.actions.declare_file(ctx.attr.name + "_corpus/corpus.log")
    ctx.actions.run(
        executable = ctx.executable.libraries_tool,
        arguments = ["resolve", ctx.file.names.path, ctx.file.corpus.path, corpus.path],
        inputs = [ctx.file.names, ctx.file.corpus],
        outputs = [corpus],
        mnemonic = "MetalSeedCorpus",
        progress_message = "Resolving the shader corpus's libraries",
    )

    out = ctx.actions.declare_directory(ctx.attr.name)
    args = ctx.actions.args()
    args.add("run")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--tool", tool)
    args.add("--mock", mock)
    args.add("--nvk", twin)
    args.add("--qc", ctx.file.qc)
    args.add("--qc-sha256", ctx.attr.ipsw[IpswInfo].derived["quartzcore_metallib_sha256"])
    args.add("--root", _repo_root(ctx.files.libraries) + "/root")
    args.add_all(ctx.files.embedded, before_each = "--embedded")
    args.add_all(ctx.files.device_libraries, before_each = "--data")
    args.add("--corpus", corpus)
    if ctx.files.records:
        args.add("--records", _repo_root(ctx.files.records) + "/records")
    args.add("--out", out.path)
    ctx.actions.run(
        executable = seed_tool,
        arguments = [args],
        inputs = depset(
            [tool, mock, twin, ctx.file.qc, corpus] + xcode + ctx.files.libraries + ctx.files.embedded +
            ctx.files.device_libraries + ctx.files.records,
        ),
        outputs = [out],
        mnemonic = "MetalSeed",
        progress_message = "Precompiling the m2v shader cache seed (corpus %s)" % ctx.file.corpus.short_path,
        # Runs the precompile tool on this Mac (Metal + the NVK twin): never on a remote executor.
        execution_requirements = {"no-remote-exec": "1"},
        resource_set = _all_cores,
    )
    return [
        DefaultInfo(files = depset([out])),
        # The steps before the run (bazel build //gpu:seed --output_groups=steps).
        OutputGroupInfo(steps = depset([mock, tool, twin, corpus])),
    ]

metal_seed = rule(
    implementation = _metal_seed_impl,
    attrs = _XCODE | {
        "host_bridge": attr.label(mandatory = True, allow_single_file = True),
        "mock_iokit": attr.label(mandatory = True, allow_single_file = True),
        "abi_header": attr.label(mandatory = True, allow_single_file = True),
        "precompile": attr.label(mandatory = True, allow_single_file = True),
        "frontend": attr.label(mandatory = True, allow_single_file = True),
        "metal_header": attr.label(mandatory = True, allow_single_file = True),
        "hdrs": attr.label_list(allow_files = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True),
        "twin": attr.label(mandatory = True, allow_single_file = True),
        "nvk_sources": attr.label_list(allow_files = True, doc = "the two dylibs' .source files"),
        "qc": attr.label(mandatory = True, allow_single_file = True, doc = "the IPSW's QuartzCore default.metallib"),
        "ipsw": attr.label(default = "//bazel/ipsw:current", providers = [IpswInfo], doc = "the QuartzCore metallib's pin"),
        "libraries": attr.label(mandatory = True, doc = "the OS volume's Metal libraries (@gpu_apple_libs//:libraries)"),
        "names": attr.label(mandatory = True, allow_single_file = [".json"], doc = "their names.json (m2v_library_names)"),
        "embedded": attr.label_list(
            allow_files = [".h"],
            doc = "headers of metallibs the frontend makes libraries of (a C array of the bytes), stored by their sha256",
        ),
        "device_libraries": attr.label(allow_files = True, doc = "libraries only the device made (@gpu_device_data)"),
        "records": attr.label(allow_files = True, doc = "the device's driver records (@gpu_device_data): merged where of the twin's identity"),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "libraries_tool": attr.label(
            mandatory = True,
            executable = True,
            cfg = "exec",
            doc = "precompile/libraries.py's py_binary (its resolve step)",
        ),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
    },
)

# --- The directory deploy stages ---------------------------------------------------------------------------------

def _gpu_tree_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.name)
    bundle = [f for f in ctx.files.bundle if f.is_directory][0]
    args = ctx.actions.args()
    args.add("--bundle", bundle.path)
    args.add("--seed", ctx.file.seed.path)
    args.add("--trustcache", ctx.file.trustcache)
    args.add("--trust-json", ctx.file.trust_json)
    if ctx.file.raw:
        args.add("--raw", ctx.file.raw.path)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [bundle, ctx.file.seed, ctx.file.trustcache, ctx.file.trust_json] + ctx.files.raw,
        outputs = [out],
        mnemonic = "GpuTree",
        progress_message = "Laying out the GPU bundle directory",
    )
    return [DefaultInfo(files = depset([out]))]

gpu_tree = rule(
    implementation = _gpu_tree_impl,
    attrs = {
        "bundle": attr.label(mandatory = True),
        "seed": attr.label(mandatory = True, allow_single_file = True),
        "trustcache": attr.label(mandatory = True, allow_single_file = True),
        "trust_json": attr.label(mandatory = True, allow_single_file = True),
        "raw": attr.label(allow_single_file = True, doc = "the raw path's pack directory (pack.bin)"),
        "_tool": attr.label(default = "//tools/gpu:assemble", executable = True, cfg = "exec"),
    },
)
