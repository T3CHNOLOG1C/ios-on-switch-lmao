"""Write out/native-a57/root-v1/tree/compile_commands.json for clangd, from the kernel's compile actions.

    bazel run //tools/lint:compdb

Builds //:kernel, asks Bazel for its KernelCompile actions (bazel aquery), and has tools/kernel/compile.py print each
one's exact command (--dry-run); adds the Metal bundle's two Objective-C++ units. Apple clang options upstream
clang (clangd) refuses or that only matter for code generation are dropped. Interim: once the drivers are cc_library
targets (r17-bazel), clang-tidy runs as a Bazel aspect and this database can come from the cc actions instead.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path.cwd())
TREE = ROOT / 'out/native-a57/root-v1/tree'
SOURCES = ('.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm', '.inc')

DROP = {'-fsanitize-minimal-runtime', '-fno-c++-static-destructors', '-fc++-static-destructors=none'}
DROP_PREFIX = (
    '-fsanitize',
    '-flto',
    '-mcpu=',
    '-march=',
    '-fptrauth',
    '-mllvm',
    '-ftrivial-auto-var-init',
    '-fbounds-safety',
    '-fexperimental-bounds-safety',
)
DROP_WITH_VALUE = {'-MF', '-MT', '-MQ', '-o', '-arch', '-Xclang', '-mllvm', '-index-store-path'}
DROP_ALONE = {'-MD', '-MMD', '-c', '-Werror'}
# Apple clang's XNU type-system builtins (kalloc_type, IOMallocType) take a type; upstream clang has none of them.
KERNEL_COMPAT = [
    '-D__builtin_xnu_type_signature(t)="ioswitch-lint"',
    '-D__builtin_xnu_type_summary(t)=0u',
    '-D__builtin_xnu_types_compatible(a,b)=1',
    '-Wno-unknown-warning-option',
    '-Wno-missing-designated-field-initializers',
]


def run(argv: list[str], **kwargs: object) -> str:
    """Run argv in the workspace and return its standard output."""
    return subprocess.run(argv, cwd=ROOT, check=True, capture_output=True, text=True, **kwargs).stdout


def clang(name: str) -> str:
    """Return Xcode's clang or clang++ (the compiler the kernel builds with)."""
    path = shutil.which(name)
    if not path:
        sys.exit(f'{name} not found (install Xcode)')
    return path


def clean(args: list[str], source: str) -> list[str]:
    """Drop the options clangd cannot use from one compile command."""
    out: list[str] = []
    skip = False
    for arg in args:
        if skip:
            skip = False
        elif arg in DROP_WITH_VALUE:
            skip = True
        elif not (arg in DROP or arg in DROP_ALONE or arg.startswith(DROP_PREFIX) or arg == source):
            out.append(arg)
    return out


def kernel_entries() -> list[dict[str, object]]:
    """Return one entry per KernelCompile action of //:kernel, as tools/kernel/compile.py runs it."""
    bazel = os.environ.get('BAZEL', 'bazel')
    subprocess.run([bazel, 'build', '//:kernel'], cwd=ROOT, check=True)
    execroot = run([bazel, 'info', 'execution_root']).strip()
    query = run([bazel, 'aquery', 'mnemonic("KernelCompile", deps(//:kernel))', '--output=jsonproto'])
    entries = []
    for action in json.loads(query).get('actions', []):
        result = subprocess.run(
            [*action['arguments'], '--dry-run'], cwd=execroot, capture_output=True, text=True, check=False
        )
        if result.returncode:
            continue
        entry = json.loads(result.stdout)
        source = entry['file']
        if not source.endswith(SOURCES):
            continue
        # The execution root links back to the workspace: name the workspace's own file, which editors open.
        relative = os.path.relpath(source, execroot)
        if not relative.startswith(('..', 'external/', 'bazel-out/')):
            source = str(ROOT / relative)
        args = [*clean(entry['arguments'][1:], entry['file']), *KERNEL_COMPAT]
        entries.append({'directory': entry['directory'], 'file': source, 'arguments': [clang('clang'), *args, source]})
    return entries


def metal_entries() -> list[dict[str, object]]:
    """Return the Metal bundle's Objective-C++ units, compiled for iOS 17 against Xcode's SDK."""
    sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    metal = TREE / 'drivers/NativeMetal'
    flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-std=c++17']
    flags += ['-fobjc-arc', '-Wno-deprecated-declarations', '-I', str(metal), '-x', 'objective-c++']
    units = [TREE / 'drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm', metal / 'NativeBridgeMetalFrontend.mm']
    return [
        {'directory': str(TREE), 'file': str(unit), 'arguments': [clang('clang++'), *flags, str(unit)]}
        for unit in units
        if unit.exists()
    ]


def main() -> int:
    """Write the database and say how many units it has."""
    entries = kernel_entries() + metal_entries()
    target = TREE / 'compile_commands.json'
    target.write_text(json.dumps(entries, indent=1) + '\n')
    print(f'{target}: {len(entries)} units')
    return 0


if __name__ == '__main__':
    sys.exit(main())
