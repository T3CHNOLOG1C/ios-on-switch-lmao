"""The shader cache seed: the host tools (built here, or the release's prebuilt ones), and what they make from the
IPSW's Metal libraries on the Mac that composes the image.

  seed_tools   precompile + mock IOKit + NVK twin + the raw pack's probe, mock and ycbcr.metallib, MANIFEST.json (Xcode)
  shader_crawl every Metal library of the IPSW kind's volume images (crawl.py)
  metal_seed   resolve the corpus, plan the shards, one precompile action per shard, merge (no compiler: the tools run)
  seed_raw_pack, seed_tree   the raw pack, and both laid out as the Data volume holds them
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")

_XCODE = {
    "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
    "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
}

_DARWIN = {"no-remote-exec": "1", "requires-darwin": ""}

def _root(files, marker):
    """The directory holding `marker` among `files`: a tree artifact, or a filegroup's files below one directory."""
    for f in files:
        if f.basename == marker:
            return f.dirname
    trees = [f for f in files if f.is_directory]
    if len(trees) != 1:
        fail("expected one directory, or files including {}".format(marker))
    return trees[0].path

def _repo_root(files):
    return files[0].owner.workspace_root

CrawlInfo = provider(
    doc = "A crawl of the IPSW's images: the library tree (root/, data/) and the QuartzCore library in it.",
    fields = {"tree": "the directory artifact", "qc": "QuartzCore's default.metallib, as a path below the tree"},
)

_QC = "root/System/Library/Frameworks/QuartzCore.framework/default.metallib"

def _shader_crawl_impl(ctx):
    tree = ctx.actions.declare_directory(ctx.attr.name)
    inventory = ctx.actions.declare_file(ctx.attr.name + ".inventory.json")
    names = ctx.actions.declare_file(ctx.attr.name + ".names.json")
    args = ctx.actions.args()
    args.add_all(ctx.files.images, before_each = "--image")
    args.add("--out", tree.path)
    args.add("--inventory", inventory)
    ctx.actions.run(
        executable = ctx.executable._crawl,
        arguments = [args],
        inputs = ctx.files.images,
        outputs = [tree, inventory],
        mnemonic = "ShaderCrawl",
        progress_message = "Crawling the IPSW's images for Metal libraries",
        execution_requirements = _DARWIN,
    )
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["names", tree.path + "/root", names.path],
        inputs = [tree],
        outputs = [names],
        mnemonic = "M2vLibraryNames",
        progress_message = "Indexing the crawled libraries' function names",
    )
    return [
        DefaultInfo(files = depset([tree, inventory])),
        CrawlInfo(tree = tree, qc = _QC),
        OutputGroupInfo(names = depset([names]), inventory = depset([inventory])),
    ]

shader_crawl = rule(
    implementation = _shader_crawl_impl,
    doc = "Every Metal library of the images (the OS volume, then the cryptexes grafted over it), and an inventory.",
    attrs = {
        "images": attr.label_list(mandatory = True, allow_files = True, doc = "@ipsw//:os, :system_cryptex, :app_cryptex"),
        "_crawl": attr.label(default = "//tools/gpu/shaders:crawl", executable = True, cfg = "exec"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
    },
)

def _seed_tools_impl(ctx):
    xcode = [ctx.file._xcode_env, ctx.file._xcode_identity]
    tool = ctx.executable._tool
    steps = {}
    for step, inputs, extra in (
        ("mock", [ctx.file.abi_header, ctx.file.mock_iokit], ["--abi-header", ctx.file.abi_header, "--mock-iokit", ctx.file.mock_iokit]),
        ("twin", [ctx.file.nvk, ctx.file.twin] + ctx.files.nvk_sources, ["--nvk", ctx.file.nvk, "--twin", ctx.file.twin]),
    ):
        out = ctx.actions.declare_file("{}_{}/{}".format(ctx.attr.name, step, "libmock_iokit.dylib" if step == "mock" else "libvulkan_nouveau.dylib"))
        args = ctx.actions.args()
        args.add(step)
        if step == "mock":
            args.add("--xcode-env", ctx.file._xcode_env)
        args.add_all(extra)
        args.add("--out", out)
        ctx.actions.run(
            executable = tool,
            arguments = [args],
            inputs = inputs + (xcode if step == "mock" else []),
            outputs = [out],
            mnemonic = "SeedTools" + step.capitalize(),
            progress_message = "Building the shader tools' " + step,
            execution_requirements = _DARWIN,
        )
        steps[step] = out

    precompile = ctx.actions.declare_file(ctx.attr.name + "_tool/precompile")
    args = ctx.actions.args()
    args.add("tool")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--metal-header", ctx.file.metal_header)
    args.add("--precompile", ctx.file.precompile)
    args.add("--frontend", ctx.file.frontend)
    args.add("--host-bridge", ctx.file.host_bridge)
    args.add("--mock", steps["mock"])
    args.add("--out", precompile)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        inputs = [ctx.file.metal_header, ctx.file.precompile, ctx.file.frontend, ctx.file.host_bridge, steps["mock"]] +
                 ctx.files.hdrs + xcode,
        outputs = [precompile],
        mnemonic = "SeedToolsPrecompile",
        progress_message = "Building the shader cache precompile tool",
        execution_requirements = _DARWIN,
    )

    tree = ctx.file.raw_generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    raw = ctx.actions.declare_directory(ctx.attr.name + "_raw")
    args = ctx.actions.args()
    args.add("build")
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--host-bridge", ctx.file.host_bridge)
    args.add("--generator", ctx.file.raw_generator)
    args.add("--tree", tree)
    args.add("--out", raw.path)
    ctx.actions.run(
        executable = ctx.executable._rawpack,
        arguments = [args],
        inputs = depset([ctx.file.host_bridge, ctx.file.raw_generator] + xcode + [f for f in ctx.files.raw_srcs if f.extension != "py"]),
        outputs = [raw],
        mnemonic = "SeedToolsRaw",
        progress_message = "Building the raw pack's probe and mock GM20B",
        execution_requirements = _DARWIN,
    )

    out = ctx.actions.declare_directory(ctx.attr.name)
    args = ctx.actions.args()
    args.add("layout")
    args.add("--precompile", precompile)
    args.add("--mock", steps["mock"])
    args.add("--twin-dylib", steps["twin"])
    args.add("--raw", raw.path)
    args.add("--stack", ctx.file.stack)
    args.add("--nvk-source", [f for f in ctx.files.nvk_sources if f.basename.endswith(".source")][0])
    args.add("--out", out.path)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        inputs = [precompile, steps["mock"], steps["twin"], raw, ctx.file.stack] + ctx.files.nvk_sources,
        outputs = [out],
        mnemonic = "SeedToolsLayout",
        progress_message = "Laying out the shader tools",
    )
    return [DefaultInfo(files = depset([out]))]

seed_tools = rule(
    implementation = _seed_tools_impl,
    doc = "The host tools the seed runs, as one directory (the release ships it prebuilt).",
    attrs = _XCODE | {
        "abi_header": attr.label(mandatory = True, allow_single_file = True),
        "mock_iokit": attr.label(mandatory = True, allow_single_file = True),
        "metal_header": attr.label(mandatory = True, allow_single_file = True),
        "precompile": attr.label(mandatory = True, allow_single_file = True),
        "frontend": attr.label(mandatory = True, allow_single_file = True),
        "hdrs": attr.label_list(allow_files = True),
        "host_bridge": attr.label(mandatory = True, allow_single_file = True),
        "nvk": attr.label(mandatory = True, allow_single_file = True, doc = "the iOS dylib, whose identity the twin takes"),
        "twin": attr.label(mandatory = True, allow_single_file = True),
        "nvk_sources": attr.label_list(allow_files = True, doc = "the two dylibs' .source files"),
        "raw_generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "raw_srcs": attr.label_list(allow_files = True),
        "stack": attr.label(mandatory = True, allow_single_file = True, doc = "the raw path's stack key"),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
        "_rawpack": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
    },
)

def _tools_archive_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.name + ".tar")
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = ["archive", "--tools", _root(ctx.files.tools, "MANIFEST.json"), "--out", out.path],
        inputs = ctx.files.tools,
        outputs = [out],
        mnemonic = "SeedToolsArchive",
        progress_message = "Archiving the shader tools",
    )
    return [DefaultInfo(files = depset([out]))]

tools_archive = rule(
    implementation = _tools_archive_impl,
    doc = "The tools directory as a tar (sorted, no times or owners): the release asset.",
    attrs = {
        "tools": attr.label(mandatory = True),
        "_tool": attr.label(default = "//tools/gpu:seed", executable = True, cfg = "exec"),
    },
)

def _metal_seed_impl(ctx):
    crawl = ctx.attr.crawl[CrawlInfo]
    names = ctx.attr.crawl[OutputGroupInfo].names.to_list()[0]
    inventory = ctx.attr.crawl[OutputGroupInfo].inventory.to_list()[0]
    qc = crawl.tree.path + "/" + crawl.qc
    corpus = ctx.actions.declare_file(ctx.attr.name + "_plan/corpus.log")
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["resolve", names.path, ctx.file.corpus.path, corpus.path],
        inputs = [names, ctx.file.corpus],
        outputs = [corpus],
        mnemonic = "MetalSeedCorpus",
        progress_message = "Resolving the shader corpus's libraries",
    )

    count = ctx.attr.shards
    shards = [ctx.actions.declare_file("{}_plan/shard-{}.log".format(ctx.attr.name, _two(i))) for i in range(count)]
    store = ctx.actions.declare_file(ctx.attr.name + "_plan/store.json")
    report = ctx.actions.declare_file(ctx.attr.name + "_plan/plan.json")
    root = crawl.tree.path + "/root"
    args = ctx.actions.args()
    args.add("--corpus", corpus)
    args.add("--root", root)
    args.add("--data-dir", crawl.tree.path + "/data")
    args.add_all(ctx.files.device_libraries, before_each = "--data")
    args.add("--qc", qc)
    args.add("--qc-sha256", ctx.attr.ipsw[IpswInfo].derived["quartzcore_metallib_sha256"])
    args.add("--depth", ctx.attr.depth[BuildSettingInfo].value)
    args.add("--shards", shards[0].dirname)
    args.add("--count", count)
    args.add("--store", store)
    args.add("--report", report)
    ctx.actions.run(
        executable = ctx.executable._plan,
        arguments = [args],
        inputs = depset([corpus, crawl.tree] + ctx.files.device_libraries),
        outputs = shards + [store, report],
        mnemonic = "MetalSeedPlan",
        progress_message = "Planning the shader seed's shards",
    )

    tools = _root(ctx.files.tools, "MANIFEST.json")
    made = []
    for i, shard in enumerate(shards):
        out = ctx.actions.declare_directory("{}_shards/{}".format(ctx.attr.name, _two(i)))
        args = ctx.actions.args()
        args.add("--tools", tools)
        args.add("--root", root)
        args.add("--store", store)
        args.add_all(ctx.files.embedded, before_each = "--embedded")
        args.add("--qc", qc)
        args.add("--lines", shard)
        if ctx.attr.depth[BuildSettingInfo].value == "deep":
            args.add("--guess-dispatch")
        args.add("--out", out.path)
        ctx.actions.run(
            executable = ctx.executable._precompile,
            arguments = [args],
            inputs = depset([shard, store, crawl.tree] + ctx.files.tools + ctx.files.device_libraries + ctx.files.embedded),
            outputs = [out],
            mnemonic = "MetalSeedShard",
            progress_message = "Precompiling shader seed shard {} of {}".format(i + 1, count),
            execution_requirements = _DARWIN,
            resource_set = _shard_resources,
        )
        made.append(out)

    seed = ctx.actions.declare_directory(ctx.attr.name)
    args = ctx.actions.args()
    args.add("seed")
    args.add_all(made, before_each = "--shard", expand_directories = False)
    if ctx.files.records:
        args.add("--records", _repo_root(ctx.files.records) + "/records")
    args.add("--tools", tools)
    args.add_all(ctx.files.base, before_each = "--base", expand_directories = False)
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add("--plan", report)
    args.add("--inventory", inventory)
    args.add("--out", seed.path)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = depset(made + [report, inventory] + ctx.files.records + ctx.files.tools + ctx.files.base),
        outputs = [seed],
        mnemonic = "MetalSeedMerge",
        progress_message = "Joining the shader seed's shards",
    )
    verify = ctx.actions.declare_directory(ctx.attr.name + "_verify")
    args = ctx.actions.args()
    args.add("--tools", tools)
    args.add("--root", root)
    args.add("--store", store)
    args.add_all(ctx.files.embedded, before_each = "--embedded")
    args.add("--qc", qc)
    args.add("--lines", corpus)
    args.add("--seed", seed.path)
    args.add("--out", verify.path)
    ctx.actions.run(
        executable = ctx.executable._precompile,
        arguments = [args],
        inputs = depset([corpus, store, seed, crawl.tree] + ctx.files.tools + ctx.files.device_libraries + ctx.files.embedded),
        outputs = [verify],
        mnemonic = "MetalSeedVerify",
        progress_message = "Replaying the shader corpus over the seed",
        execution_requirements = _DARWIN,
    )
    return [
        DefaultInfo(files = depset([seed])),
        OutputGroupInfo(plan = depset([corpus, store, report] + shards), shards = depset(made), verify = depset([verify])),
    ]

def _two(i):
    return ("0" + str(i))[-2:]

def _shard_resources(_os, _inputs):
    return {"cpu": 1, "memory": 2048}

metal_seed = rule(
    implementation = _metal_seed_impl,
    doc = "The m2v shader cache seed the device reads beneath every process's own cache.",
    attrs = {
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo], doc = "the IPSW's libraries (shader_crawl)"),
        "device_libraries": attr.label(allow_files = True, doc = "libraries only the device made (@gpu_device_data)"),
        "records": attr.label(allow_files = True, doc = "the device's driver records (@gpu_device_data)"),
        "embedded": attr.label_list(allow_files = [".h"], doc = "C arrays of metallibs the frontend makes libraries of"),
        "tools": attr.label(mandatory = True, doc = "the host tools (a directory, or files with MANIFEST.json)"),
        "base": attr.label(mandatory = True, doc = "the GPU bundle the seed is for (//gpu:base or the release's)"),
        "depth": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "shards": attr.int(default = 16),
        "ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo]),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
        "_plan": attr.label(default = "//tools/gpu/shaders:plan", executable = True, cfg = "exec"),
        "_precompile": attr.label(default = "//tools/gpu/shaders:precompile", executable = True, cfg = "exec"),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)

def _seed_raw_pack_impl(ctx):
    tree = ctx.file.generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    out = ctx.actions.declare_directory(ctx.attr.name)
    args = ctx.actions.args()
    args.add("run")
    crawl = ctx.attr.crawl[CrawlInfo]
    args.add("--shader-tools", _root(ctx.files.tools, "MANIFEST.json"))
    args.add("--qc", crawl.tree.path + "/" + crawl.qc)
    args.add("--corpus", ctx.file.corpus)
    args.add("--generator", ctx.file.generator)
    args.add("--tree", tree)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset([crawl.tree, ctx.file.corpus, ctx.file.generator] + ctx.files.tools + ctx.files.srcs),
        outputs = [out],
        mnemonic = "MetalRawPack",
        progress_message = "Making the raw GM20B path's pipeline pack",
        execution_requirements = _DARWIN,
    )
    return [DefaultInfo(files = depset([out]))]

seed_raw_pack = rule(
    implementation = _seed_raw_pack_impl,
    doc = "The raw GM20B path's pipeline pack: Core Animation's corpus states through the tools' probe over the mock GM20B.",
    attrs = {
        "tools": attr.label(mandatory = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the generator's Python modules"),
        "_tool": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
    },
)

def _seed_tree_impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.name)
    size = ctx.actions.declare_file(ctx.attr.name + ".size.json")
    args = ctx.actions.args()
    args.add("tree")
    args.add("--seed", ctx.file.seed.path)
    if ctx.file.raw:
        args.add("--raw", ctx.file.raw.path)
    args.add("--out", out.path)
    args.add("--size", size)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [ctx.file.seed] + ctx.files.raw,
        outputs = [out, size],
        mnemonic = "MetalSeedTree",
        progress_message = "Laying out the shader seed for the Data volume",
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(size = depset([size]))]

seed_tree = rule(
    implementation = _seed_tree_impl,
    doc = "ngpu/addons/TegraGM20BMetal.m2v-seed/shared and TegraGM20BMetal.raw, for the Data volume's root.",
    attrs = {
        "seed": attr.label(mandatory = True, allow_single_file = True),
        "raw": attr.label(allow_single_file = True),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)
