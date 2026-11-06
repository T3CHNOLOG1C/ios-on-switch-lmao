"""XNU's compile and link records as the tables kernel/xnu-<version>/ compiles from.

The tables are <component>/objects.bzl and config/recipes_<config>.bzl (the forms xnu_objects.bzl and xnu_kernel.bzl
read).

Input is a VERBOSE=YES make build of the tree (real compilers): every compile's -MJ record in the object root, each
component's <component>.filelist for the link order, and from the log the setsegname loops and the kernel's link.

usage: xnu_compile_record.py --log MAKE.LOG --obj OBJROOT --src SRC --sdkroot SDKROOT --tree TREE --config CONFIG
                             --out KERNEL/XNU-<VERSION>
"""

import argparse
import collections
import json
import re
import shlex
from collections.abc import Iterable
from pathlib import Path

COMPONENTS = ['osfmk', 'bsd', 'iokit', 'libkern', 'libsa', 'pexpert', 'san', 'security']
LANGS = {'c': 'c', 'cpp': 'cxx', 's': 'asm'}
JOINED = ('-I', '-D', '-U')
SEGMENT_LOOP = re.compile(r'^for (\w+)_file in (.*?)\s*;\s*(?:do)?')
SEGMENT_STEP = re.compile(r'setsegname (.*?) -o \$\{(\w+)_file\}__ \$\{\2_file\}')


class Places:
    """Absolute paths of the build as the tables' placeholders, longest first."""

    def __init__(self, args: argparse.Namespace, objdir: str):
        obj = Path(args.obj).resolve()
        pairs = [
            (str(Path(args.src).resolve()), '{xnu}'),
            (str(obj / 'EXPORT_HDRS'), '{export}'),
            (str(obj / 'san'), '{san}'),
            (str(obj / objdir), '{objdir}'),
            (str(Path(args.sdkroot).resolve()), '{sdkroot}'),
            (str(Path(args.tree).resolve()), '{tree}'),
        ]
        self.pairs = sorted(pairs, key=lambda p: -len(p[0]))

    def spell(self, token: str, gen: str | None) -> str:
        """Token with every absolute root as its placeholder; gen (the compile's directory) as {gen}."""
        for root, name in [(gen, '{gen}'), *self.pairs]:
            token = token.replace(root + '/', name + '/').replace(root, name) if root else token
        return token


def split_joined(tokens: Iterable[str]) -> list[str]:
    """-Ifoo -> -I foo, -DX -> -D X, -UX -> -U X (the tables' spelling)."""
    out = []
    for t in tokens:
        if t.startswith(JOINED) and len(t) > 2 and not t.startswith('-Wl'):
            out += [t[:2], t[2:]]
        else:
            out.append(t)
    return out


def compiles(obj: str) -> list[tuple[Path, str, str, list[str]]]:
    """Return every compile's -MJ record in obj: (the directory it ran in, its source, its arguments).

    The records keep make's argument order less the dependency flags (-MD, -MF, -MJ, -MP), plus what the clang driver
    appends to every compile (vendor features, the linker version, the target): that common suffix is dropped, the
    kernel toolchain adds its own.
    """
    found = []
    for path in sorted(Path(obj).rglob('*.json')):
        text = path.read_text().strip().rstrip(',')
        if text.startswith('{'):
            record = json.loads(text)
            found.append((path, record['directory'], record['file'], record['arguments']))
    suffix = min(len(r[3]) for r in found)
    while suffix and len({tuple(r[3][-suffix:]) for r in found}) != 1:
        suffix -= 1
    return [(path, cwd, source, argv[: len(argv) - suffix]) for path, cwd, source, argv in found]


def parse_compile(argv: list[str], source: str, places: Places, gen: str) -> tuple[str, list[str], list[str]]:
    """(language, flags up to INCFLAGS, INCFLAGS and the object's own) of one compile record."""
    rest = [t for t in argv[1:] if t != source]
    if '-o' in rest:
        out = rest.index('-o')
        rest = rest[:out] + rest[out + 2 :]
    lang = LANGS[source.rpartition('.')[2]]
    rest = split_joined(rest)
    inc = next(i for i in range(len(rest) - 1) if rest[i] == '-I' and rest[i + 1] == '.')
    flags, includes = rest[:inc], rest[inc:]

    def spell(tokens: list[str]) -> list[str]:
        out = []
        for token in split_joined(tokens):
            if token == '.':
                t = '{gen}/.'
            elif token == 'meta_features.h':
                t = '{gen}/meta_features.h'
            else:
                t = token
            out.append(places.spell(t, gen))
        return out

    return lang, spell(flags), drop_missing_sdk_includes(spell(includes))


def drop_missing_sdk_includes(tokens: list[str]) -> list[str]:
    """Drop the iPhoneOS SDK's iBoot and AppleFeatures include directories make names: the SDK has neither."""
    out, i = [], 0
    while i < len(tokens):
        if tokens[i] == '-I' and i + 1 < len(tokens) and '/Platforms/iPhoneOS.platform/' in tokens[i + 1]:
            i += 2
            continue
        out.append(tokens[i])
        i += 1
    return out


def segments(log: str) -> dict[str, list[str]]:
    """Object name -> setsegname argument strings, in order (the loops make runs after a component's compiles)."""
    out = collections.defaultdict(list)
    files = {}
    for line in Path(log).read_text(errors='replace').splitlines():
        m = SEGMENT_LOOP.match(line.strip())
        if m:
            files[m.group(1)] = m.group(2).split()
            continue
        m = SEGMENT_STEP.search(line)
        if m:
            for f in files.get(m.group(2), []):
                out[f].append(m.group(1))
    return out


def link(log: str, places: Places) -> list[str]:
    """Return the kernel's link (MakeInc.kernel), its archives (what follows -lcc_kext) as @KERNEL_ARCHIVES@."""
    for line in Path(log).read_text(errors='replace').splitlines():
        if '-filelist link.filelist' not in line or '.unstripped' not in line:
            continue
        argv = shlex.split(line)[1:]
        argv = [*argv[: argv.index('-lcc_kext') + 1], '@KERNEL_ARCHIVES@']
        return [places.spell(t, None) for t in argv]
    raise SystemExit('no kernel link in the log')


def gen_root(cwd: str, objdir: str) -> Path:
    """Return the directory a component's generated sources hang from: its <component>/<CONFIG>, or the config's."""
    path = Path(cwd).resolve()
    while objdir not in (path.name, path.parent.parent.name):
        path = path.parent
    return path


def common_prefix(values: list[list[str]]) -> list[str]:
    """Return the longest list every value starts with: a component's recipe is what all its objects share."""
    first = values[0]
    n = len(first)
    for v in values[1:]:
        n = min(n, len(v))
        while v[:n] != first[:n]:
            n -= 1
    return first[:n]


def bzl_list(values: Iterable[str], indent: int) -> str:
    pad = ' ' * indent
    return '[\n' + ''.join(f'{pad}    {_q(v)},\n' for v in values) + f'{pad}]'


def _q(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def write_objects(
    path: Path,
    comp: str,
    config: str,
    objects: list[str],
    copts: dict[str, list[str]],
    includes: dict[str, list[str]],
    segs: dict[str, list[str]],
) -> None:
    doc = (
        f'"""XNU\'s {comp} objects for each XNU config, in link order (make\'s {comp}.filelist), and what each one adds'
        " to its\ncomponent's recipe: copts (conf/Makefile.*'s <object>_CFLAGS_ADD and _CWARNFLAGS_ADD), includes"
        " (<object>_INCFLAGS,\nafter the recipe's) and segments (the setsegname passes make runs on the object).\n"
        '"gen:" sources are generated (MIG, iig, makesyscalls, newvers) in the component\'s object directory.\n'
        'Generated by kernel/rules/xnu_compile_record.py."""\n\n'
    )

    def table(d: dict[str, list[str]]) -> str:
        if not d:
            return '{}'
        body = ''.join(f'        {_q(k)}: {bzl_list(v, 8)},\n' for k, v in sorted(d.items()))
        return '{\n' + body + '    }'

    text = doc + f'{config} = struct(\n    objects = {bzl_list(objects, 4)},\n'
    text += f'    copts = {table(copts)},\n    includes = {table(includes)},\n    segments = {table(segs)},\n)\n'
    path.write_text(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('log', 'obj', 'src', 'sdkroot', 'tree', 'config', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    config = args.config
    objdir = f'{config}_ARM64_BCM2837'
    places = Places(args, objdir)
    src_root = str(Path(args.src).resolve())
    by_object = {}
    for path, cwd, source, argv in compiles(args.obj):
        gen = str(Path(cwd).resolve())
        lang, flags, inc = parse_compile(argv, source, places, gen)
        comp = 'config' if gen.endswith('/' + objdir) else Path(gen).parent.name
        resolved = str(Path(cwd, source).resolve())
        if resolved.startswith(src_root + '/'):
            entry = resolved[len(src_root) + 1 :]
        else:
            entry = 'gen:' + str(Path(resolved).relative_to(gen_root(cwd, objdir)))
        by_object[(comp, path.name[: -len('.json')])] = (entry, lang, flags, inc)
    segs = segments(args.log)
    recipes = {}
    for comp in [*COMPONENTS, 'config']:
        if comp == 'config':
            names = ['lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o', 'version.o']
        else:
            filelist = Path(args.obj) / objdir / comp / config / f'{comp}.filelist'
            names = [Path(p).name for p in filelist.read_text().split()]
        rows = [(n, *by_object[(comp, n)]) for n in names]
        key = 'kernel' if comp == 'config' else comp
        for lang in sorted({r[2] for r in rows}):
            mine = [r for r in rows if r[2] == lang]
            recipes[f'{key}_{lang}'] = (common_prefix([r[3] for r in mine]), common_prefix([r[4] for r in mine]))
        objects, copts, includes, seg = [], {}, {}, {}
        for name, entry, lang, flags, inc in rows:
            base_flags, base_inc = recipes[f'{key}_{lang}']
            if flags[: len(base_flags)] != base_flags:
                raise SystemExit(f'{comp} {name}: flags differ from the {key}_{lang} recipe beyond its additions')
            if inc[: len(base_inc)] != base_inc:
                raise SystemExit(f'{comp} {name}: includes differ from the {key}_{lang} recipe')
            objects.append(entry)
            if flags[len(base_flags) :]:
                copts[entry] = flags[len(base_flags) :]
            if inc[len(base_inc) :]:
                includes[entry] = inc[len(base_inc) :]
            if name in segs:
                seg[entry] = segs[name]
        out_dir = Path(args.out) / comp
        write_objects(out_dir / 'objects.bzl', comp, config, objects, copts, includes, seg)
    lines = [
        (
            f'"""XNU\'s compile recipes for config {config}: per component and language, CFLAGS+CWARNFLAGS (flags) and'
            ' INCFLAGS\n(includes). Generated by kernel/rules/xnu_compile_record.py."""'
        ),
        '',
        f'{config}_RECIPES = {{',
    ]
    for k in sorted(recipes):
        flags, inc = recipes[k]
        lines.append(
            f'    {_q(k)}: struct(\n        flags = {bzl_list(flags, 8)},\n'
            f'        includes = {bzl_list(inc, 8)},\n    ),'
        )
    lines.append('}')
    lines += ['', f'{config}_LINK = {bzl_list(link(args.log, places), 0)}']
    (Path(args.out) / 'config' / f'recipes_{config.lower()}.bzl').write_text('\n'.join(lines) + '\n')
    print(f'{len(by_object)} compiles, {len(recipes)} recipes')


if __name__ == '__main__':
    main()
