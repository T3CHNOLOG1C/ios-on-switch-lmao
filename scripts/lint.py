#!/usr/bin/env python3
"""clang-format and clang-tidy for our sources in the kernel tree (out/native-a57/root-v1/tree).

  scripts/lint.py compdb                  write <tree>/compile_commands.json (clang-tidy, clangd)
  scripts/lint.py format [--check] FILE…  clang-format in place (--check: report, change nothing)
  scripts/lint.py tidy [--fix] FILE…      clang-tidy with <tree>/.clang-tidy
  scripts/lint.py format|tidy --all       every first-party file instead of a list
  scripts/lint.py churn [DIR…]            lines clang-format would change, per directory (nothing written)

Files are paths relative to the project root, the tree, or absolute. Formatting follows the nearest .clang-format:
the tree root's, a directory's own where its style differs, and DisableFormat where the code is someone else's
(XNU, nouveau, Linux ports). `compdb` reads the CMake build's commands (cmake -S . -B out/cmake -G Ninja first) and
adds the Metal bundle's two Objective-C++ units, which build-tegra-gm20b-metal.py compiles outside it.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / 'out/native-a57/root-v1/tree'
BUILD = ROOT / 'out/cmake'
LLVM = Path('/opt/homebrew/opt/llvm/bin')
SOURCES = ('.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm', '.inc')
FIRST_PARTY = ('drivers', 'kits', 'diagnostics/ioswitch')
# Not ours, or generated: never formatted or linted (formatting is also switched off by their .clang-format).
FOREIGN = re.compile(r'/(build|\.cache|refer|nouveau|linux|corpus|hostsim|__pycache__)/|\.(orig|rej)$|/test/vectors/')

# Apple clang options upstream clang refuses or that only matter for code generation.
DROP = {'-fsanitize-minimal-runtime', '-fno-c++-static-destructors',
        '-fc++-static-destructors=none'}
DROP_PREFIX = ('-fsanitize', '-flto', '-mcpu=', '-march=', '-fptrauth', '-mllvm', '-ftrivial-auto-var-init',
               '-fbounds-safety', '-fexperimental-bounds-safety')
DROP_WITH_VALUE = {'-MF', '-MT', '-MQ', '-o', '-arch', '-Xclang', '-mllvm', '-index-store-path'}
# Apple clang's XNU type-system builtins (kalloc_type, IOMallocType) take a type; upstream clang has none of them.
# For analysis only, macros that drop the type stand in (they never reach a real build).
KERNEL_COMPAT = ['-D__builtin_xnu_type_signature(t)="ioswitch-lint"', '-D__builtin_xnu_type_summary(t)=0u',
                 '-D__builtin_xnu_types_compatible(a,b)=1', '-Wno-unknown-warning-option',
                 '-Wno-missing-designated-field-initializers']


def tool(name):
    path = LLVM / name if (LLVM / name).exists() else shutil.which(name)
    if not path:
        sys.exit(f'{name} not found (brew install llvm)')
    return str(path)


def expand(args, directory):
    out = []
    for arg in args:
        if arg.startswith('@'):
            rsp = Path(arg[1:])
            out += expand(shlex.split((rsp if rsp.is_absolute() else Path(directory) / rsp).read_text()), directory)
        else:
            out.append(arg)
    return out


def clean(args, source):
    out, skip = [], False
    for arg in args:
        if skip:
            skip = False
        elif arg in DROP_WITH_VALUE:
            skip = True
        elif arg in DROP or arg.startswith(DROP_PREFIX) or arg in ('-MD', '-MMD', '-c', '-Werror') or arg == source:
            continue
        else:
            out.append(arg)
    return out


def ninja_entries():
    if not (BUILD / 'build.ninja').exists():
        sys.exit(f'{BUILD}/build.ninja missing: run cmake -S . -B out/cmake -G Ninja')
    raw = json.loads(subprocess.run(['ninja', '-C', str(BUILD), '-t', 'compdb'], check=True, capture_output=True,
                                    text=True).stdout)
    entries = []
    for e in raw:
        source = e.get('file', '')
        if not source.endswith(SOURCES) or source.endswith('.S'):
            continue
        source = str((Path(e['directory']) / source).resolve())
        directory, command = e['directory'], None
        for part in e['command'].split(' && '):   # cd <dir> && <cc> … && <depfile transform>
            words = shlex.split(part)
            if words and words[0] == 'cd':
                directory = words[1]
            elif words and re.search(r'(^|/)(clang|clang\+\+|cc|c\+\+)$', words[0]) and source in words:
                command = words
        if not command:
            continue
        args = clean(expand(command[1:], directory), source) + KERNEL_COMPAT
        entries.append({'directory': directory, 'file': source, 'arguments': [tool('clang')] + args + [source]})
    return entries


def metal_entries():
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True,
                         text=True).stdout.strip()
    metal = TREE / 'drivers/NativeMetal'
    flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-std=c++17', '-fobjc-arc',
             '-Wno-deprecated-declarations', '-I', str(metal), '-x', 'objective-c++']
    units = [TREE / 'drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm', metal / 'NativeBridgeMetalFrontend.mm']
    return [{'directory': str(TREE), 'file': str(u), 'arguments': [tool('clang++')] + flags + [str(u)]}
            for u in units if u.exists()]


def compdb():
    entries = ninja_entries() + metal_entries()
    target = TREE / 'compile_commands.json'
    target.write_text(json.dumps(entries, indent=1) + '\n')
    print(f'{target}: {len(entries)} units')
    return entries


def resolve(paths):
    out = []
    for p in paths:
        for base in (Path.cwd(), ROOT, TREE):
            candidate = (base / p).resolve()
            if candidate.exists():
                out.append(candidate)
                break
        else:
            sys.exit(f'{p}: not found')
    return out


def first_party(roots=FIRST_PARTY):
    for r in roots:
        for d, dirs, files in os.walk(TREE / r):
            dirs[:] = sorted(x for x in dirs if not FOREIGN.search(f'/{x}/'))
            for f in sorted(files):
                p = Path(d) / f
                if f.endswith(SOURCES) and not FOREIGN.search(str(p)):
                    yield p


def formatted(path):
    run = subprocess.run([tool('clang-format'), '--style=file', '--fallback-style=none', str(path)], capture_output=True,
                         text=True)
    return run.returncode, run.stdout, run.stderr


def changed_lines(path):
    code, out, _ = formatted(path)
    if code:
        return None
    before = path.read_text(errors='replace').splitlines()
    import difflib
    return sum(1 for l in difflib.unified_diff(before, out.splitlines(), n=0, lineterm='')
               if l.startswith('-') and not l.startswith('---'))


def cmd_format(args):
    files = list(first_party()) if args.all else resolve(args.files)
    bad = 0
    for f in files:
        if args.check:
            n = changed_lines(f)
            if n:
                print(f'{f.relative_to(TREE) if f.is_relative_to(TREE) else f}: {n} lines would change')
                bad += 1
        else:
            subprocess.run([tool('clang-format'), '-i', '--style=file', '--fallback-style=none', str(f)], check=True)
    if args.check:
        print(f'{bad} of {len(files)} files need formatting')
    return 1 if bad else 0


def cmd_tidy(args):
    db = TREE / 'compile_commands.json'
    if not db.exists() or args.compdb:
        compdb()
    known = {e['file'] for e in json.loads(db.read_text())}
    files = [f for f in (first_party() if args.all else resolve(args.files)) if f.suffix not in ('.h', '.hpp', '.inc')]
    missing = [f for f in files if str(f) not in known]
    for f in missing:
        print(f'{f}: not in compile_commands.json (not built by out/cmake); skipped', file=sys.stderr)
    files = [f for f in files if str(f) in known]
    base = [tool('clang-tidy'), '-p', str(TREE), '--quiet'] + (['--fix'] if args.fix else [])

    def one(f):
        r = subprocess.run(base + [str(f)], capture_output=True, text=True)
        return f, r.returncode, r.stdout + r.stderr

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for f, code, text in pool.map(one, files):
            text = '\n'.join(l for l in text.splitlines() if not re.match(r'^\d+ warnings? (and \d+ errors? )?generated', l))
            if text.strip():
                print(text)
            failed += code != 0 or ': warning:' in text or ': error:' in text
    print(f'clang-tidy: {failed} of {len(files)} files with findings')
    return 1 if failed else 0


def cmd_churn(args):
    roots = args.dirs or [str(p.relative_to(TREE)) for r in FIRST_PARTY for p in sorted((TREE / r).iterdir())
                          if p.is_dir() and not FOREIGN.search(f'/{p.name}/')] if not args.dirs else args.dirs
    for r in roots:
        files = list(first_party([r]))
        if not files:
            continue
        total = sum(len(f.read_text(errors='replace').splitlines()) for f in files)
        changed = sum(changed_lines(f) or 0 for f in files)
        print(f'{r:40} {changed:6} of {total:6} lines ({100 * changed / max(total, 1):5.1f}%)')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('compdb')
    f = sub.add_parser('format')
    f.add_argument('--check', action='store_true')
    f.add_argument('--all', action='store_true')
    f.add_argument('files', nargs='*')
    t = sub.add_parser('tidy')
    t.add_argument('--fix', action='store_true')
    t.add_argument('--all', action='store_true')
    t.add_argument('--compdb', action='store_true', help='regenerate compile_commands.json first')
    t.add_argument('-j', '--jobs', type=int, default=os.cpu_count())
    t.add_argument('files', nargs='*')
    c = sub.add_parser('churn')
    c.add_argument('dirs', nargs='*')
    a = p.parse_args()
    if a.cmd in ('format', 'tidy') and not a.all and not a.files:
        p.error('give files, or --all')
    sys.exit({'compdb': lambda: compdb() and 0, 'format': lambda: cmd_format(a), 'tidy': lambda: cmd_tidy(a),
              'churn': lambda: cmd_churn(a)}[a.cmd]())


if __name__ == '__main__':
    main()
