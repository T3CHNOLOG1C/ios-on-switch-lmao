"""The shader cache seed: what the host tools (tools.bzl) make from the IPSW's Metal libraries on the Mac that composes
the image.

  shader_crawl   every Metal library of the IPSW kind's volume images (crawl.py)
  metal_seed     resolve the corpus, plan the buckets, one precompile action per bucket, join their indexes
  seed_raw_pack  the raw GM20B path's pipeline pack, from Core Animation's corpus lines
  seed_tree      the seed and the raw pack laid out as the Data volume holds them (the image builder's)

Every output is named below the IPSW kind (21H461/, 24B119/): a build of the other kind writes its own files, so
switching --config=ios27 on and off reuses both instead of overwriting one with the other.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")
load(":tools.bzl", "seed_tool_files")

_DARWIN = {"no-remote-exec": "1", "requires-darwin": ""}

_IPSW = {"ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo])}

def _named(ctx, path):
    return "{}/{}{}".format(ctx.attr.ipsw[IpswInfo].kind, ctx.attr.name, path)

def _repo_root(files):
    return files[0].owner.workspace_root

CrawlInfo = provider(
    doc = "A crawl of the IPSW's images: the library tree (root/, data/) and the QuartzCore library in it.",
    fields = {"tree": "the directory artifact", "qc": "QuartzCore's default.metallib, as a path below the tree"},
)

SeedInfo = provider(
    doc = "The packed seed: the buckets' packs (packs/<NNN> in order) and the join's directory.",
    fields = {
        "packs": "one pack per bucket",
        "meta": "SEED, MANIFEST.json, the index and the merged driver records' pack",
    },
)

_QC = "root/System/Library/Frameworks/QuartzCore.framework/default.metallib"

def _shader_crawl_impl(ctx):
    tree = ctx.actions.declare_directory(_named(ctx, ""))
    inventory = ctx.actions.declare_file(_named(ctx, ".inventory.json"))
    names = ctx.actions.declare_file(_named(ctx, ".names.json"))
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
    attrs = _IPSW | {
        "images": attr.label_list(mandatory = True, allow_files = True, doc = "@ipsw//:os, :system_cryptex, :app_cryptex"),
        "_crawl": attr.label(default = "//tools/gpu/shaders:crawl", executable = True, cfg = "exec"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
    },
)

def _plan(ctx, crawl, corpus):
    count = ctx.attr.buckets
    buckets = [ctx.actions.declare_file(_named(ctx, "_plan/bucket-{}.log".format(_three(i)))) for i in range(count)]
    store = ctx.actions.declare_file(_named(ctx, "_plan/store.json"))
    report = ctx.actions.declare_file(_named(ctx, "_plan/plan.json"))
    args = ctx.actions.args()
    args.add("--corpus", corpus)
    args.add("--root", crawl.tree.path + "/root")
    args.add("--data-dir", crawl.tree.path + "/data")
    args.add_all(ctx.files.device_libraries, before_each = "--data")
    args.add("--qc", crawl.tree.path + "/" + crawl.qc)
    args.add("--qc-sha256", ctx.attr.ipsw[IpswInfo].derived["quartzcore_metallib_sha256"])
    args.add("--depth", ctx.attr.depth[BuildSettingInfo].value)
    args.add("--buckets", buckets[0].dirname)
    args.add("--count", count)
    args.add("--store", store)
    args.add("--report", report)
    ctx.actions.run(
        executable = ctx.executable._plan,
        arguments = [args],
        inputs = depset([corpus, crawl.tree] + ctx.files.device_libraries),
        outputs = buckets + [store, report],
        mnemonic = "MetalSeedPlan",
        progress_message = "Planning the shader seed's buckets",
    )
    return buckets, store, report

def _precompile_args(ctx, tools, crawl, store, lines, out):
    args = ctx.actions.args()
    args.add("--precompile", tools.precompile)
    args.add("--mock", tools.mock)
    args.add("--nvk", tools.twin)
    args.add("--root", crawl.tree.path + "/root")
    args.add("--store", store)
    args.add_all(ctx.files.embedded, before_each = "--embedded")
    args.add("--qc", crawl.tree.path + "/" + crawl.qc)
    args.add("--lines", lines)
    args.add("--out", out)
    return args

def _metal_seed_impl(ctx):
    crawl = ctx.attr.crawl[CrawlInfo]
    names = ctx.attr.crawl[OutputGroupInfo].names.to_list()[0]
    inventory = ctx.attr.crawl[OutputGroupInfo].inventory.to_list()[0]
    tools = seed_tool_files(ctx.attr.tools)
    corpus = ctx.actions.declare_file(_named(ctx, "_plan/corpus.log"))
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["resolve", names.path, ctx.file.corpus.path, corpus.path],
        inputs = [names, ctx.file.corpus],
        outputs = [corpus],
        mnemonic = "MetalSeedCorpus",
        progress_message = "Resolving the shader corpus's libraries",
    )
    lines, store, report = _plan(ctx, crawl, corpus)

    # A bucket's compile reads its lines, the libraries and the three tools that make code (the precompile tool with
    # the frontend and m2v in it, its mock IOKit, the NVK twin): never the plugin, the corpus or the rest of the tools.
    compile_inputs = [store, crawl.tree, tools.precompile, tools.mock, tools.twin] + ctx.files.device_libraries + ctx.files.embedded
    packs, indexes, reports = [], [], []
    for i, bucket_lines in enumerate(lines):
        out = ctx.actions.declare_file(_named(ctx, "_buckets/{}.pack".format(_three(i))))
        index = ctx.actions.declare_file(_named(ctx, "_buckets/{}.index".format(_three(i))))
        bucket_report = ctx.actions.declare_file(_named(ctx, "_buckets/{}.json".format(_three(i))))
        args = _precompile_args(ctx, tools, crawl, store, bucket_lines, out)
        if ctx.attr.depth[BuildSettingInfo].value == "deep":
            args.add("--guess-dispatch")
        args.add("--index", index)
        args.add("--report", bucket_report)
        ctx.actions.run(
            executable = ctx.executable._precompile,
            arguments = [args],
            inputs = depset([bucket_lines] + compile_inputs),
            outputs = [out, index, bucket_report],
            mnemonic = "MetalSeedBucket",
            progress_message = "Precompiling shader seed bucket {} of {}".format(i + 1, len(lines)),
            execution_requirements = _DARWIN,
            resource_set = _bucket_resources,
        )
        packs.append(out)
        indexes.append(index)
        reports.append(bucket_report)

    meta = ctx.actions.declare_directory(_named(ctx, "_meta"))
    args = ctx.actions.args()
    args.add("join")
    args.add_all(indexes, before_each = "--index")
    args.add_all(reports, before_each = "--report")
    if ctx.files.records:
        args.add("--records", _repo_root(ctx.files.records) + "/records")
    args.add("--tools-manifest", tools.manifest)
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add("--plan", report)
    args.add("--inventory", inventory)
    args.add("--out", meta.path)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = depset(indexes + reports + [tools.manifest, report, inventory] + ctx.files.records),
        outputs = [meta],
        mnemonic = "MetalSeedJoin",
        progress_message = "Joining the shader seed's bucket indexes",
    )

    # The tools must be the GPU bundle's, or no key of the seed would ever be hit: checked beside the build, which
    # neither waits for it nor reruns anything when only the bundle changes.
    checked = ctx.actions.declare_file(_named(ctx, ".tools-match"))
    args = ctx.actions.args()
    args.add("check")
    args.add("--tools-manifest", tools.manifest)
    args.add("--twin", tools.twin)
    args.add_all(ctx.files.base, before_each = "--base", expand_directories = False)
    args.add("--out", checked)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [tools.manifest, tools.twin] + ctx.files.base,
        outputs = [checked],
        mnemonic = "MetalSeedCheck",
        progress_message = "Checking the shader tools are the GPU bundle's",
    )

    verify = ctx.actions.declare_file(_named(ctx, "_verify/misses.pack"))
    verify_index = ctx.actions.declare_file(_named(ctx, "_verify/misses.index"))
    verify_report = ctx.actions.declare_file(_named(ctx, "_verify/report.json"))
    args = _precompile_args(ctx, tools, crawl, store, corpus, verify)
    args.add("--index", verify_index)
    args.add("--report", verify_report)
    args.add("--seed", meta.path)
    args.add_all(packs, before_each = "--seed-pack")
    ctx.actions.run(
        executable = ctx.executable._precompile,
        arguments = [args],
        inputs = depset([corpus, meta] + packs + compile_inputs),
        outputs = [verify, verify_index, verify_report],
        mnemonic = "MetalSeedVerify",
        progress_message = "Replaying the shader corpus over the seed",
        execution_requirements = _DARWIN,
    )
    return [
        DefaultInfo(files = depset([meta] + packs)),
        SeedInfo(packs = packs, meta = meta),
        OutputGroupInfo(
            plan = depset([corpus, store, report] + lines),
            buckets = depset(indexes + reports),
            verify = depset([verify_report]),
            _validation = depset([checked]),
        ),
    ]

def _three(i):
    return ("00" + str(i))[-3:]

def _bucket_resources(_os, _inputs):
    return {"cpu": 1, "memory": 1024}

metal_seed = rule(
    implementation = _metal_seed_impl,
    doc = "The m2v shader cache seed the device reads beneath every process's own cache.",
    attrs = _IPSW | {
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo], doc = "the IPSW's libraries (shader_crawl)"),
        "device_libraries": attr.label(allow_files = True, doc = "libraries only the device made (@gpu_device_data)"),
        "records": attr.label(allow_files = True, doc = "the device's driver records (@gpu_device_data)"),
        "embedded": attr.label_list(allow_files = [".h"], doc = "C arrays of metallibs the frontend makes libraries of"),
        "tools": attr.label(mandatory = True, doc = "the host tools (seed_tools, or files with MANIFEST.json)"),
        "base": attr.label(mandatory = True, doc = "the GPU bundle the seed is for (//gpu:base or the release's)"),
        "depth": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "buckets": attr.int(default = 256, doc = "a corpus line costs its bucket's compile: a 256th of the seed"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
        "_plan": attr.label(default = "//tools/gpu/shaders:plan", executable = True, cfg = "exec"),
        "_precompile": attr.label(default = "//tools/gpu/shaders:precompile", executable = True, cfg = "exec"),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)

def _seed_raw_pack_impl(ctx):
    tree = ctx.file.generator.dirname.partition("drivers/TegraGM20BMetal/")[0].rstrip("/") or "."
    tools = seed_tool_files(ctx.attr.tools)
    crawl = ctx.attr.crawl[CrawlInfo]
    lines = ctx.actions.declare_file(_named(ctx, "_corpus.log"))
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = ["corpus", "--corpus", ctx.file.corpus.path, "--out", lines.path],
        inputs = [ctx.file.corpus],
        outputs = [lines],
        mnemonic = "MetalRawCorpus",
        progress_message = "Picking the raw pack's corpus lines",
    )
    out = ctx.actions.declare_directory(_named(ctx, ""))
    args = ctx.actions.args()
    args.add("run")
    args.add("--nvk", tools.twin)
    args.add("--raw", tools.raw_dir)
    args.add("--manifest", tools.manifest)
    args.add("--qc", crawl.tree.path + "/" + crawl.qc)
    args.add("--corpus", lines)
    args.add("--generator", ctx.file.generator)
    args.add("--tree", tree)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset([crawl.tree, lines, ctx.file.generator, tools.twin, tools.manifest] + tools.raw + ctx.files.srcs),
        outputs = [out],
        mnemonic = "MetalRawPack",
        progress_message = "Making the raw GM20B path's pipeline pack",
        execution_requirements = _DARWIN,
    )
    return [DefaultInfo(files = depset([out]))]

seed_raw_pack = rule(
    implementation = _seed_raw_pack_impl,
    doc = "The raw GM20B path's pipeline pack: Core Animation's corpus states through the tools' probe over the mock GM20B.",
    attrs = _IPSW | {
        "tools": attr.label(mandatory = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the generator's Python modules"),
        "_tool": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
    },
)

def _seed_tree_impl(ctx):
    seed = ctx.attr.seed[SeedInfo]
    out = ctx.actions.declare_directory(_named(ctx, ""))
    size = ctx.actions.declare_file(_named(ctx, ".size.json"))
    args = ctx.actions.args()
    args.add("tree")
    args.add("--seed", seed.meta.path)
    args.add_all(seed.packs, before_each = "--pack")
    if ctx.file.raw:
        args.add("--raw", ctx.file.raw.path)
    args.add("--out", out.path)
    args.add("--size", size)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [seed.meta] + seed.packs + ctx.files.raw,
        outputs = [out, size],
        mnemonic = "MetalSeedTree",
        progress_message = "Laying out the shader seed for the Data volume",
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(size = depset([size]))]

seed_tree = rule(
    implementation = _seed_tree_impl,
    doc = "ngpu/addons/TegraGM20BMetal.m2v-seed/shared and TegraGM20BMetal.raw, for the Data volume's root.",
    attrs = _IPSW | {
        "seed": attr.label(mandatory = True, providers = [SeedInfo]),
        "raw": attr.label(allow_single_file = True),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)
