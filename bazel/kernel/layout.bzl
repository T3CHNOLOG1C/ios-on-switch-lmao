"""Where the kernel's sources and generated files live, and how their paths are spelled in what the build ships.

The layout move (S3) took the kernel tree's drivers/, kits/, diagnostics/ and overlay/ from out/native-a57/root-v1/tree/
to the repository root (same relative layout) and its generated files from that package's build/ to kernel/build/.
Paths reach the artifacts (__FILE__ strings, debug info, the kernel's stabs): COMPAT_* keep the spellings the kernel
had before the move, so the move changes no shipped byte but the linker's object paths. Dropping the compatibility
spellings is a later, separate change (old_spelling and the compat maps become identities).

tools/kernel/common.py mirrors this table for the tools (keep the two in step).
"""

PATH_ROOT = "/ioswitch"

# The kernel tree's location before the move: the old spelling of every moved path.
OLD_TREE = "out/native-a57/root-v1/tree"

# Where the kernel tree's sources are now ("" = the repository root), and the package of its generated files.
TREE_ROOT = OLD_TREE
KERNEL_PACKAGE = OLD_TREE

# The tree's top-level directories that live at TREE_ROOT.
MOVED_ROOTS = ["drivers", "kits", "diagnostics", "overlay"]

def tree_path(path):
    """A path below the kernel tree, as it is now (TREE_ROOT/path)."""
    return (TREE_ROOT + "/" + path) if TREE_ROOT else path

def old_spelling(path, bin_dir = None):
    """The pre-move spelling of an execroot-relative path (unchanged if it was not moved)."""
    for root in MOVED_ROOTS:
        if path == root or path.startswith(root + "/"):
            return OLD_TREE + "/" + path
    if bin_dir and KERNEL_PACKAGE != OLD_TREE:
        new = bin_dir + "/" + KERNEL_PACKAGE + "/"
        if path.startswith(new):
            return bin_dir + "/" + OLD_TREE + "/" + path[len(new):]
    return path

def compat_prefix_maps(bin_dir):
    """Prefix maps from moved paths to their old spellings: macro maps (the most specific prefix wins) and debug maps
    (the last match wins, so these go after the generic ones). Inert while nothing lives at the new paths."""
    macro, debug = [], []
    if TREE_ROOT != OLD_TREE:
        for root in MOVED_ROOTS:
            macro.append("-fmacro-prefix-map={r}/={p}/{o}/{r}/".format(r = root, p = PATH_ROOT, o = OLD_TREE))
            debug.append("-fdebug-prefix-map={r}/={p}/{o}/{r}/".format(r = root, p = PATH_ROOT, o = OLD_TREE))
    if KERNEL_PACKAGE != OLD_TREE:
        new = "{}/{}/build".format(bin_dir, KERNEL_PACKAGE)
        macro.append("-fmacro-prefix-map={}={}/{}/build".format(new, PATH_ROOT, OLD_TREE))
        debug.append("-fdebug-prefix-map={}={}/{}/{}/build".format(new, PATH_ROOT, bin_dir, OLD_TREE))
    return macro, debug
