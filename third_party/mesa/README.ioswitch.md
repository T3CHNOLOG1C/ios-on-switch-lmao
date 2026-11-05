# Mesa, vendored for NVK

Upstream: <https://gitlab.freedesktop.org/mesa/mesa>, commit `77452b9092cc44d76d92180a12d3aaaf27a98382`.

What is here: the parts of that commit the NVK build reads, byte for byte as `git archive` exports them (the
`.csv` files with CRLF line ends, as Mesa's `.gitattributes` checks them out): `.gitattributes`, `README.rst`,
`VERSION`, `meson.build`, `meson.options`, `rustfmt.toml`, `licenses/`, `docs/license.rst`, `bin/`, `include/`,
`src/meson.build`, `src/c11/`, `src/util/`, `src/compiler/`, `src/vulkan/`, `src/nouveau/` and `subprojects/` (the
wrap files, and the six crates NAK's wraps fetch, extracted where meson extracts them: `subprojects/<crate>-<version>/`,
from crates.io by the wraps' own sha256). Nothing else of Mesa is built. Mesa's licences are in `licenses/` and
`docs/license.rst`; every file keeps its own copyright and SPDX notice, and each crate its LICENSE files.

ioswitch's changes are the commits on top of the import (`git log -- third_party/mesa`), one per change, each with
its reason. The Bazel build (`BUILD.bazel` files beside the `meson.build` files NVK's build uses, and `bazel/`) is
ioswitch's own and is not part of Mesa: one action per compile and per generator, meson's flags and orders exactly
(bazel/defs.bzl explains the path conventions), so the dylibs are the ones meson makes, byte for byte.

Moving to a new Mesa commit: on a branch, replace the paths above with the new commit's (`git archive <commit>
<paths> | tar -x -C third_party/mesa`), commit that as the import, rebase ioswitch's Mesa commits onto it, then
update the BUILD files where Mesa's meson.build files changed (new sources, generators or flags). Compare against a
meson build of the same tree (its compile_commands.json, ninja's `-t commands`): the objects and archives must match.
