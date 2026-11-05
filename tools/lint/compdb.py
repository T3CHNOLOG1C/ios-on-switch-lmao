"""Write compile_commands.json (for clangd) at the repository root from the build's real compile actions.

    bazel run //tools/lint:compdb                  ours and XNU's units
    bazel run //tools/lint:compdb -- --vendored    also third_party/ (Mesa) and generated sources

Asks `bazel aquery` (analysis only: nothing is built) for every C, C++ and Objective-C(++) compile of //:boot, //:gpu
and every cc_test: the kernel's drivers, kits and XNU with the kernel toolchain's flags and XNU's SDK include paths,
the device user space with the iOS SDK, the host tests with googletest's includes. Each unit gets the command its
action runs, made usable from the workspace root:

- bazel-out/ paths go through the bazel-<workspace> link to the execution root, external/ ones to the output base's
  external/ (the execution root links an external repository only once an action has used it); Xcode's placeholders
  (__BAZEL_XCODE_SDKROOT__, @IPHONEOS_SDK@, ...) become this machine's paths;
- dropped: what only the build needs (-MD/-MF, -o, prefix maps, sanitizer ignore lists, -Werror) and Apple clang's
  private -Xclang options,
  which an upstream clangd refuses; Apple's XNU type builtins get stand-ins for the same reason.

Headers the build generates (XNU's exported headers) are found once `bazel build //:boot` has made them.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path.cwd())
QUERY = (
    'mnemonic("CppCompile|CCompile|ObjcCompile|ObjcxxCompile|ObjcppCompile", '
    'deps(//:boot) union deps(//:gpu) union kind("cc_test", //...))'
)
SOURCES = ('.c', '.cc', '.cpp', '.cxx', '.m', '.mm')
VENDORED = ('third_party/', 'bazel-out/', 'external/')
DROP = {'-MD', '-MMD', '-Werror', '-fcolor-diagnostics', '-no-canonical-prefixes'}
DROP_WITH_VALUE = {'-MF', '-MT', '-MQ', '-o', '-index-store-path'}
DROP_PREFIX = (
    '-fdebug-prefix-map=',
    '-fmacro-prefix-map=',
    '-ffile-prefix-map=',
    '-fdebug-compilation-dir=',
    '-fsanitize-ignorelist=',
    '-Werror=',
)
# Apple clang's XNU type-system builtins (kalloc_type, IOMallocType) take a type; upstream clangd has none of them.
KERNEL_COMPAT = [
    '-D__builtin_xnu_type_signature(t)="switch-lint"',
    '-D__builtin_xnu_type_summary(t)=0u',
    '-D__builtin_xnu_types_compatible(a,b)=1',
    '-Wno-unknown-warning-option',
]


def run(*argv: str) -> str:
    """Run a command in the workspace and return its standard output."""
    return subprocess.run(argv, cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def unit_of(arguments: list[str]) -> str:
    """Return the unit an action compiles: the argument after its last -c."""
    last = len(arguments) - 1 - arguments[::-1].index('-c')
    return arguments[last + 1]


def rewrite(argument: str, roots: dict[str, str], places: dict[str, str]) -> str:
    """Return one argument with Xcode's placeholders filled in and its build-tree paths made workspace-relative."""
    for placeholder, value in places.items():
        argument = argument.replace(placeholder, value)
    argument = re.sub(r'@FILE:[^@]*@', '0', argument)
    return re.sub(
        r'(^|=|^-I|^-F|^-iquote|^-isystem|^-idirafter)(external|bazel-out)/',
        lambda m: m.group(1) + roots[m.group(2)] + '/',
        argument,
    )


def clean(arguments: list[str], roots: dict[str, str], places: dict[str, str], compilers: dict[str, str]) -> list[str]:
    """Return a compile command clangd can use from the workspace root."""
    if '--' in arguments:  # a wrapper (xcode_run ... -- clang++ ...): the real command follows
        arguments = arguments[arguments.index('--') + 1 :]
    out = []
    skip = False
    for index, argument in enumerate(arguments):
        if skip:
            skip = False
            continue
        if argument in DROP or argument.startswith(DROP_PREFIX):
            continue
        if argument in DROP_WITH_VALUE or (argument == '-Xclang' and index + 1 < len(arguments)):
            skip = True  # -Xclang passes Apple clang's private cc1 options
            continue
        out.append(rewrite(argument, roots, places))
    compiler = Path(out[0]).name
    out[0] = compilers['clang++'] if compiler.endswith(('++', '_pp')) else compilers['clang']
    if '-mkernel' in out:
        out[1:1] = KERNEL_COMPAT
    return out


def main() -> int:
    """Query the compile actions, rewrite them for clangd, and say how many units there are."""
    vendored = '--vendored' in sys.argv[1:]
    bazel = os.environ.get('BAZEL', 'bazel')
    link = f'bazel-{ROOT.name}'
    if not (ROOT / link).is_dir():
        sys.exit(f'{ROOT / link}: missing (it appears after the first `bazel build` in this checkout)')
    developer = run('xcode-select', '-p')
    places = {
        '__BAZEL_XCODE_SDKROOT__': run('xcrun', '--sdk', 'macosx', '--show-sdk-path'),
        '__BAZEL_XCODE_DEVELOPER_DIR__': developer,
        '/PLACEHOLDER_DEVELOPER_DIR': developer,
        '__BAZEL_EXECUTION_ROOT__': link,
        '@IPHONEOS_SDK@': run('xcrun', '--sdk', 'iphoneos', '--show-sdk-path'),
        '@PWD@': '.',
    }
    compilers = {'clang': run('xcrun', '-f', 'clang'), 'clang++': run('xcrun', '-f', 'clang++')}
    roots = {'bazel-out': f'{link}/bazel-out', 'external': run(bazel, 'info', 'output_base') + '/external'}
    actions = json.loads(run(bazel, 'aquery', QUERY, '--output=jsonproto', '--include_artifacts=false') or '{}')
    entries = {}
    for action in actions.get('actions', []):
        unit = unit_of(action['arguments'])
        if not unit.endswith(SOURCES) or (unit.startswith(VENDORED) and not vendored):
            continue
        arguments = clean(action['arguments'], roots, places, compilers)
        if unit.startswith(('external/', 'bazel-out/')):
            top, _, rest = unit.partition('/')
            arguments = [a if a != unit else f'{roots[top]}/{rest}' for a in arguments]
            unit = f'{roots[top]}/{rest}'
        # A unit built in two configurations keeps its device (kernel) command: that is the code that ships.
        if unit in entries and '-mkernel' in entries[unit]['arguments']:
            continue
        entries[unit] = {'directory': str(ROOT), 'file': unit, 'arguments': arguments}
    target = ROOT / 'compile_commands.json'
    target.write_text(json.dumps(sorted(entries.values(), key=lambda e: e['file']), indent=1) + '\n')
    print(f'{target}: {len(entries)} units')
    return 0


if __name__ == '__main__':
    sys.exit(main())
