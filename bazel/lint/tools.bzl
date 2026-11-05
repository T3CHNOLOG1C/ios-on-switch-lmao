"""The lint and format tools (//bazel/lint), each an official release binary for macOS arm64 pinned by version and sha256.

clang-format and clang-tidy come from @llvm (LLVM 23.1.4, bazel/llvm), rustfmt from the Rust 1.98.0 toolchain
(rules_rust) and uv from rules_python's uv extension (MODULE.bazel). The versions match the Homebrew copies the editor
uses (ruff 0.17.3, buildifier 10.2.0), so the editor and `bazel test` agree.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

_GH = "https://github.com/"

# name: (url, sha256, strip_prefix or None for a bare binary, binary name)
ARCHIVES = {
    "dprint": (
        _GH + "dprint/dprint/releases/download/0.64.3/dprint-aarch64-apple-darwin.zip",
        "94b48820945602fb113a34343db0a9e653a8346c60966f790556dc9ce4084a92",
        "",
        "dprint",
    ),
    "ruff": (
        _GH + "astral-sh/ruff/releases/download/0.17.3/ruff-aarch64-apple-darwin.tar.gz",
        "118f7121aa838785672bf612487f755a89108bb5f44e0f3df96c5967a72614f9",
        "ruff-aarch64-apple-darwin",
        "ruff",
    ),
    "rumdl": (
        _GH + "rvben/rumdl/releases/download/v0.3.2/rumdl-v0.3.2-aarch64-apple-darwin.tar.gz",
        "a1eefb1cd2e0cafa42f0ca98d3f8713d7e73f9eba77fc8ef0ed6836875197935",
        "",
        "rumdl",
    ),
    "shellcheck": (
        _GH + "koalaman/shellcheck/releases/download/v0.11.0/shellcheck-v0.11.0.darwin.aarch64.tar.gz",
        "339b930feb1ea764467013cc1f72d09cd6b869ebf1013296ba9055ab2ffbd26f",
        "shellcheck-v0.11.0",
        "shellcheck",
    ),
}

FILES = {
    "buildifier": (
        _GH + "bazelbuild/buildtools/releases/download/v10.2.0/buildifier-darwin-arm64",
        "d9c9c3a2826d1d8f592a9d463e62aed7f4dcacc5ab48e8de6be53c420b34b971",
        True,
    ),
    # dprint's formatters are WebAssembly plugins; dprint gets these paths with --plugins and never downloads.
    "dprint_plugin_json": (
        _GH + "dprint/dprint-plugin-json/releases/download/0.25.2/plugin.wasm",
        "811ca4c6f732bcd6f707bc2f42d42395bb4127cdf829960cf628a18fc149777e",
        False,
    ),
    "dprint_plugin_toml": (
        _GH + "dprint/dprint-plugin-toml/releases/download/0.9.0/plugin.wasm",
        "776e088ed4255a51a80e3640e5fd20b15661147e86df3e99611aaa37e9c59852",
        False,
    ),
    "shfmt": (
        _GH + "mvdan/sh/releases/download/v3.14.1/shfmt_v3.14.1_darwin_arm64",
        "b7c872db63553ccffc7253aba3ed7d4885a27d83f1ba567b1138c6315a5847e5",
        True,
    ),
}

def _lint_tools_impl(module_ctx):
    for name, (url, sha256, strip_prefix, binary) in ARCHIVES.items():
        http_archive(
            name = name,
            url = url,
            sha256 = sha256,
            strip_prefix = strip_prefix,
            build_file_content = 'exports_files(["{}"], visibility = ["//visibility:public"])\n'.format(binary),
        )
    for name, (url, sha256, executable) in FILES.items():
        http_file(
            name = name,
            url = url,
            sha256 = sha256,
            executable = executable,
            downloaded_file_path = name if executable else name + ".wasm",
        )
    return module_ctx.extension_metadata(reproducible = True)

lint_tools = module_extension(implementation = _lint_tools_impl)
