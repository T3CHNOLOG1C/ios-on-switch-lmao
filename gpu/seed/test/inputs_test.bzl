"""Analysis tests of what the seed's actions are keyed on, over stand-in inputs (no IPSW is fetched):

- the crawl reads the images and the crawler, and the crawler's own runfiles are crawl.py and metallib.py;
- a bucket's compile reads its bucket's lines, the libraries and the three tools that make code: not the corpus, the
  GPU bundle, the tools' manifest or the raw pack's tools, so a corpus edit reaches only the buckets it rewrites and a
  plugin edit reaches none;
- the join reads the buckets' indexes and reports, never their packs;
- the bundle check is a validation action, so the GPU bundle is no input of anything that makes the seed.
"""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _actions(env, mnemonic):
    return [a for a in analysistest.target_actions(env) if a.mnemonic == mnemonic]

def _names(action):
    return [f.short_path for f in action.inputs.to_list()]

def _crawl_impl(ctx):
    env = analysistest.begin(ctx)
    (crawl,) = _actions(env, "ShaderCrawl")
    images = [p for p in _names(crawl) if "/images/" in p]
    asserts.equals(env, 3, len(images), "the three images")
    stray = [p for p in _names(crawl) if "/images/" not in p and not p.startswith(("tools/gpu/shaders/", "../"))]
    asserts.equals(env, [], stray, "the crawl's inputs beyond the images and the crawler")
    return analysistest.end(env)

_crawl_test = analysistest.make(_crawl_impl)

def _crawler_impl(ctx):
    env = analysistest.begin(ctx)
    runfiles = analysistest.target_under_test(env)[DefaultInfo].default_runfiles.files.to_list()
    sources = [f for f in runfiles if f.is_source and f.owner.workspace_name == "" and f.extension == "py"]
    own = sorted([f.short_path for f in sources])
    asserts.equals(env, ["tools/gpu/shaders/crawl.py", "tools/gpu/shaders/metallib.py"], own)
    return analysistest.end(env)

_crawler_test = analysistest.make(_crawler_impl)

def _seed_impl(ctx):
    env = analysistest.begin(ctx)
    buckets = _actions(env, "MetalSeedBucket")
    asserts.equals(env, 4, len(buckets))
    for i, bucket in enumerate(buckets):
        names = _names(bucket)
        lines = [p for p in names if p.endswith(".log")]
        asserts.equals(env, 1, len(lines), "one bucket's lines")
        asserts.true(env, lines[0].endswith("bucket-00{}.log".format(i)), lines[0])
        for forbidden in ("corpus.log", "/base/", "MANIFEST.json", "/raw/"):
            asserts.equals(env, [], [p for p in names if forbidden in p], "a bucket's inputs naming " + forbidden)
    (join,) = _actions(env, "MetalSeedJoin")
    asserts.equals(env, [], [p for p in _names(join) if p.endswith(".pack")], "the join reads no pack")
    asserts.equals(env, [], [p for p in _names(join) if "/base/" in p], "the join reads no GPU bundle")
    (check,) = _actions(env, "MetalSeedCheck")
    asserts.true(env, [p for p in _names(check) if "/base/" in p] != [], "the bundle is checked")
    validation = analysistest.target_under_test(env)[OutputGroupInfo]._validation.to_list()
    asserts.equals(env, check.outputs.to_list(), validation, "the check is a validation action")
    return analysistest.end(env)

_seed_test = analysistest.make(_seed_impl)

def seed_inputs_tests(name, crawl, crawler, seed):
    """The tests, as one test_suite.

    Args:
        name: the test_suite's name.
        crawl: a shader_crawl over stand-in images.
        crawler: //tools/gpu/shaders:crawl.
        seed: a metal_seed of four buckets over stand-in inputs.
    """
    _crawl_test(name = name + "_crawl", target_under_test = crawl)
    _crawler_test(name = name + "_crawler", target_under_test = crawler)
    _seed_test(name = name + "_seed", target_under_test = seed)
    native.test_suite(name = name, tests = [name + "_crawl", name + "_crawler", name + "_seed"])
