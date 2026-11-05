"""The symbol names XNU's kernel defines, as a list the archives linked beside it namespace against.

One line per name (kernel_names.py's), "g <name>" for a global, "l <name>" for a local.

An XNU change that adds or removes no symbol leaves this list, and so corecrypto's archive, as it was: Bazel then
does not rebuild corecrypto (41 s) after an edit inside an XNU function.

The native XNU build lists each object's names in an action of its own (//kernel/xnu-10063.141.1/config:link_symbols:
"g <name>" from nm -g -U -j, "a <name>" from nm -U -j), so after an edit only that object is listed again; this merges
those lists (named one per line in --names-list) into the globals, then every other name. all-alias.exp's lines are
"<alias>  <target>" (config/generate_linker_aliases.sh, from the exports' "<alias>:<target>" entries): the link
creates the alias and the target is an exported kernel symbol, so both words are names of the kernel, and globals,
as kernel_names() counted them.

usage: xnu_symbols.py --names-list LIST --aliases all-alias.exp --out NAMES.txt
"""

import argparse
from pathlib import Path


def main() -> None:
    """Merge the per-object name lists and the alias list into the kernel's names."""
    parser = argparse.ArgumentParser()
    for name in ('names-list', 'aliases', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    globals_, every = set(), set()
    for path in Path(args.names_list).read_text().split():
        for line in Path(path).read_text().splitlines():
            kind, _, name = line.partition(' ')
            (globals_ if kind == 'g' else every).add(name)
    aliases = Path(args.aliases).read_text().splitlines()
    globals_ |= {word for line in aliases for word in line.split() if word.startswith('_')}
    every |= globals_
    lines = ['g ' + name for name in sorted(globals_)] + ['l ' + name for name in sorted(every - globals_)]
    Path(args.out).write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
