"""Files Bazel takes from outside its own fetches (the checked-in pins, the GPU's prebuilt host inputs), each checked
against its sha256 by Bazel itself: a repository rule copies it in with rctx.download from a file:// URL and the
pinned sha256, so a mismatch fails the fetch, and the bytes the build sees are the pinned ones (a repository cache hit
for that sha256 skips reading the file at all).

Pins are `shasum -a 256` lines (`<sha256>  <relative path>`); blank lines and # comments are skipped."""

def sha256_lines(text):
    """[(sha256, relative path)] of a `shasum -a 256` listing."""
    pins = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, _, name = line.partition("  ")
        if len(digest) != 64 or not name:
            fail("not a `shasum -a 256` line: " + line)
        pins.append((digest, name))
    return pins

def copy_checked(rctx, source, output, sha256):
    """`source` (a path outside the repository) copied to `output`, checked against `sha256`."""
    rctx.watch(source)
    rctx.download(url = "file://" + str(source), output = output, sha256 = sha256)

def _files_below(directory, prefix):
    """Relative names of every file below `directory` (Starlark has no recursion: a work list, at most 10000 dirs)."""
    found = []
    pending = [(directory, prefix)]
    for _ in range(10000):
        if not pending:
            return found
        current, name = pending.pop()
        for child in current.readdir():
            if child.is_dir:
                pending.append((child, name + child.basename + "/"))
            elif child.basename != ".DS_Store":
                found.append(name + child.basename)
    fail("too many directories below " + str(directory))

def _pinned_files_impl(rctx):
    sums = rctx.path(rctx.attr.sums)
    root = sums.dirname
    pins = sha256_lines(rctx.read(sums))
    names = []
    for digest, name in pins:
        copy_checked(rctx, root.get_child(name), name, digest)
        names.append(name)

    # Every file of the pinned directories is listed: nothing pinned goes unchecked.
    listed = {name: True for name in names}
    unlisted = []
    for directory in sorted({name.split("/")[0]: True for name in names}.keys()):
        rctx.watch_tree(root.get_child(directory))
        unlisted += [f for f in _files_below(root.get_child(directory), directory + "/") if f not in listed]
    if unlisted:
        fail("{} lists no sha256 for {}".format(rctx.attr.sums, sorted(unlisted)))
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

exports_files({names})

filegroup(
    name = "all",
    srcs = {names},
)
""".format(names = repr(sorted(names))))

pinned_files = repository_rule(
    implementation = _pinned_files_impl,
    doc = "The files a SHA256SUMS lists (paths relative to it), each checked against its sha256.",
    attrs = {
        "sums": attr.label(mandatory = True, allow_single_file = True),
    },
)
