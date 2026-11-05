"""lint_files(): the files of one package that //bazel/lint checks, chained through its subpackages.

Every BUILD file calls it once:

    load("//bazel/lint:defs.bzl", "lint_files")

    lint_files()                                     # or lint_files(exclude = ["vendored/**"])

It declares `:lint_files`, a filegroup of the package's own lintable files (by name and extension, see PATTERNS) plus
the `:lint_files` of each direct subpackage, so `//:lint_files` is every lintable file in the repository and a
package that forgets the call breaks the chain loudly ("no such target '//x:lint_files'"). Bazel can only see files
that a package declares; this is how BUILD, .bzl, Markdown, JSON and TOML files, which no rule reads, become inputs
of cached checks. Third-party and generated files go in `exclude`.
"""

PATTERNS = [
    # Starlark
    "BUILD",
    "BUILD.bazel",
    "MODULE.bazel",
    "REPO.bazel",
    "**/*.bzl",
    # Python
    "**/*.py",
    # C, C++, Objective-C, Objective-C++
    "**/*.c",
    "**/*.cc",
    "**/*.cpp",
    "**/*.h",
    "**/*.hpp",
    "**/*.inc",
    "**/*.m",
    "**/*.mm",
    # Rust, shell, Markdown, JSON, TOML
    "**/*.rs",
    "**/*.sh",
    "**/*.md",
    "**/*.json",
    "**/*.toml",
    # Tool configuration the checks read for the files below it (clang-format's and clang-tidy's nearest-file lookup)
    "**/.clang-format",
    "**/.clang-tidy",
]

# Never ours to check, wherever they turn up in a working copy: editor and tool state, build output, the clangd
# database `bazel run //tools/lint:compdb` writes.
ALWAYS_EXCLUDE = [
    "bazel-*/**",
    "**/__pycache__/**",
    "**/.ruff_cache/**",
    "**/compile_commands.json",
    "**/*.orig",
    "**/*.rej",
]

def lint_files(
        name = "lint_files",
        exclude = [],
        dirs = None,
        subpackages = None,
        visibility = ["//visibility:public"]):
    """Declares the package's lintable files and those of its subpackages as one filegroup.

    By default the whole package is globbed and its subpackages are discovered (native.subpackages), both of which
    walk every directory below the package. A package whose directory also holds big trees that are not its own
    (the root: out/'s build output and scratch, which are untracked) names what to look at instead, with `dirs` and
    `subpackages`, so loading it reads a few directories rather than walking the working copy.

    Args:
        name: the filegroup's name; the chain expects the default.
        exclude: glob patterns (relative to the package) of third-party or generated files to leave alone.
        dirs: when set, only the package's top-level files and these directories (recursively) are globbed.
        subpackages: when set, the subpackages to chain (paths relative to the package), instead of discovering them.
        visibility: the filegroup's visibility; its parent package must see it.
    """
    package = native.package_name()
    if dirs == None:
        patterns = PATTERNS
    else:
        top = [p.removeprefix("**/") for p in PATTERNS]
        patterns = top + [d + "/" + p for d in dirs for p in PATTERNS]
    if subpackages == None:
        # Bazel's convenience symlinks (bazel-bin, bazel-<workspace>, ...) lead back into the tree: never through them.
        subpackages = native.subpackages(include = ["**"], exclude = ["bazel-*/**"], allow_empty = True)
    native.filegroup(
        name = name,
        srcs = native.glob(patterns, exclude = exclude + ALWAYS_EXCLUDE, allow_empty = True) + [
            "//{}:{}".format(package + "/" + sub if package else sub, name)
            for sub in subpackages
        ],
        visibility = visibility,
    )
