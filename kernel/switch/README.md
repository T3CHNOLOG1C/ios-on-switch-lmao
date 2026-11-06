# kernel/switch

Our changes to XNU live here. Apple's files in `kernel/xnu-<version>/` carry only hooks, and both kernels compile the
code behind them from this one copy.

- `include/switch/<subsystem>.h` is what an Apple file includes. It declares the hooks, holds `static inline` bodies for
  hot paths (faults, the scheduler, page setup) and defines `SWITCH_*` macros for changes that can't be a call.
- `<subsystem>/` has the code.
- `objects.bzl` lists each source with the XNU component that compiles it and the Apple object it links after. The
  source gets that component's XNU flags, so a release build puts it in XNU's LTO and the hooks inline.

Every XNU object gets `-I kernel/switch/include` and the selected XNU's version macro, `XNU_10063_141_1` or
`XNU_12377_121_6`. Where the versions differ, the code here uses `#if XNU_<version>`.

## Adding a hook

In the Apple file, add `#include <switch/<subsystem>.h>` once, then one line per hook:

- a call: `switch_<subsystem>_<event>(...);`
- a decision: `if (switch_<subsystem>_<question>(...)) return ...;`
- a struct field, table entry, changed default or debug block forced on: a `SWITCH_*` macro from the header, so the
  Apple line keeps its shape.

Keep logic out of the Apple file and function pointers off hot paths. The code goes in `<subsystem>/` with an entry in
`objects.bzl`, and the hook lines go into every tree.

`git diff <the tree's import commit> -- kernel/xnu-<version>` lists every hook. The import commit added the tree:
`git log --diff-filter=A --format=%h -- kernel/xnu-<version>/APPLE_LICENSE`.

## Porting to a new XNU

1. Import Apple's tree unchanged in one commit.
2. Copy the hook lines from the newest tree's diff into the new tree, function by function.
3. Add `XNU_<new version>` cases here wherever Apple's code moved or changed type.
4. Build the new tree's objects. An `objects.bzl` entry whose Apple object is missing fails the build and names it.
