"""Records what XNU's make does to make its headers and generated sources: the tables kernel/rules/xnu.bzl replays as
one Bazel action per step (bazel run //kernel/xnu-<version>:update_tables, after a change to XNU's makefiles, MASTER
files, conf/files, a script they run or Xcode; bazel test //kernel/xnu-<version>:tables_test says when it is due).

In a scratch directory, on a copy of kernel/xnu-<version>, it runs make as the earlier Bazel flow did:
  installhdrs  SDKROOT=macosx into a DSTROOT that already holds the SDK root's other parts (tools/kernel/sdkroot.py):
               the kernel SDK root (Kernel.framework, System.framework, usr/include, usr/local/include)
  build        SDKROOT=iphoneos, once per kernel configuration (DEVELOPMENT, RELEASE; make's xnu_build.py), the
               compiler replaced by `false` (make -k goes on): what the build writes besides objects
Every tool that writes a header or a generated source is wrapped so that it logs its working directory, arguments and
environment, and what it wrote: unifdef, installfile, mig, iig, and the scripts (doconf, makesyscalls.sh,
make_symbol_aliasing.sh, make_posix_availability.sh, newvers.pl). The log becomes the table: per output file, the
step that writes it, with paths relative to one of these roots:
  {s}  the XNU source            {o}  the pass's object root      {d}  the pass's DSTROOT (installhdrs)
  {k}  KERNEL_SDKROOT            {x}  Xcode's DEVELOPER_DIR
and each table is checked: every file make left (less objects and what r18-xnu-objs' link rules make) has exactly one
step, and nothing else.

usage: xnu_record.py --xnu DIR --version V --darwin V --ios-min V --out DIR --availability DIR --libplatform DIR
                     --libdispatch DIR [--jobs N] [--keep DIR] [--check]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xnu_ops

DEVELOPER_DIR = os.environ.get('DEVELOPER_DIR', '/Applications/Xcode-beta.app/Contents/Developer')
TOOLCHAIN = DEVELOPER_DIR + '/Toolchains/XcodeDefault.xctoolchain/usr/bin'
SCRIPTS = [
    'SETUP/config/doconf',
    'bsd/kern/makesyscalls.sh',
    'bsd/sys/make_symbol_aliasing.sh',
    'bsd/sys/make_posix_availability.sh',
    'config/newvers.pl',
]
# What the scripts read from make's environment (grep of the five scripts); paths in them become placeholders.
SCRIPT_ENV = [
    'SRCROOT',
    'OBJROOT',
    'TARGET',
    'SDKROOT',
    'PLATFORM',
    'DRIVERKITROOT',
    'XNU_AVAILABILITY_PL',
    'CURRENT_KERNEL_CONFIG',
    'CURRENT_KERNEL_CONFIG_LC',
    'CURRENT_ARCH_CONFIG_LC',
    'CURRENT_MACHINE_CONFIG_LC',
    'RC_DARWIN_KERNEL_VERSION',
    'RC_ProjectSourceVersion',
    'MIGCC',
    'MIGCOM',
]


def build_vars(ios_min: str) -> list[str]:
    """Return the make variables of make's xnu_build.py that reach generated files.

    CFLAGS_EXTRA and the per-file flags only reach objects.
    """
    return [
        'SDKROOT=iphoneos',
        'ARCH_CONFIGS=ARM64',
        'MACHINE_CONFIGS=BCM2837',
        'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64',
        'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY',
        'ARM64_LINK_ADDR=0xfffffff007004000',
        'ARM64_PLKSEG_ADDR=0xfffffff004004000',
        f'DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min={ios_min} -DXNU_TARGET_OS_IOS',
        f'DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,{ios_min}',
        'BUILD_LTO=0',
        'BOUND_CHECKS=0',
        'BUILD_WERROR=0',
        'KCC=/usr/bin/false',
        'KC++=/usr/bin/false',
        'S_KCC=/usr/bin/false',
        'GENASSYM_KCC=/usr/bin/false',
        'LD=/usr/bin/false',
    ]


INSTALLHDRS_VARS = ['SDKROOT=macosx', 'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=DEVELOPMENT']
# Files the build leaves that are not headers or sources other code compiles: objects and their records, the host
# tools, and what the link rules (r18-xnu-objs: config/, san/, assym, version.c) make themselves.
NOT_OURS = re.compile(
    r'(\.(o|cpo|json|d|cpd|a|filelist|exp|plist|timestamp)$|^SETUP/|^san/|^cscope\.genhdrs/|'
    r'^[A-Z]+_ARM64_BCM2837/(config/|version\.c$)|/assym\.s$|(^|/)\.[^/]*$|/mach/checked$|'
    r'\.unifdef\.\d+$|\.strip\.\d+$)'
)

WRAPPER = r"""#!/usr/bin/python3 -I
import json, os, sys
LOG, TOOL, REAL, WATCH = os.environ["XNU_RECORD_LOG"], %r, %r, %r
argv = sys.argv[1:]
cwd = os.getcwd()
def snapshot(dirs):
    seen = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                p = os.path.join(root, f)
                try:
                    st = os.lstat(p)
                except OSError:
                    continue
                seen[p] = (st.st_size, st.st_mtime_ns, st.st_ino)
    return seen
# doconf: what it writes into its -d directory (the other scripts' outputs are known from their arguments).
watch = [argv[i + 1] for i, a in enumerate(argv[:-1]) if a == "-d"] if WATCH else []
before = snapshot(watch)
import re, subprocess
deps = None
if TOOL == "mig":
    # -MD: the preprocessor lists what the .defs read (<defs>.<pid>.d in the working directory).
    listed = set(os.listdir(cwd))
    r = subprocess.run([REAL, "-MD"] + argv)
    base = os.path.basename(argv[-1])[:-5]
    new = [f for f in set(os.listdir(cwd)) - listed if f.startswith(base + ".") and f.endswith(".d")]
    if len(new) == 1:
        text = open(os.path.join(cwd, new[0])).read().replace("\\\n", " ")
        deps = [t for t in text.partition(":")[2].split() if not t.endswith(".c")]
        os.unlink(os.path.join(cwd, new[0]))
elif TOOL == "iig":
    r = subprocess.run([REAL] + argv)
    clang = os.path.join(os.path.dirname(REAL), "clang")
    source = argv[argv.index("--def") + 1]
    m = subprocess.run([clang, "-M", "-MG"] + argv[argv.index("--") + 1:] + [source], capture_output=True, text=True)
    if m.returncode == 0:
        deps = m.stdout.replace("\\\n", " ").partition(":")[2].split()
else:
    r = subprocess.run([REAL] + argv)
after = snapshot(watch)
written = sorted(p for p, v in after.items() if before.get(p) != v)
env = {k: os.environ[k] for k in %r if k in os.environ}
with open(LOG, "a") as f:
    f.write(json.dumps({"tool": TOOL, "cwd": cwd, "argv": argv, "pid": os.getpid(), "ppid": os.getppid(),
                        "status": r.returncode, "written": written, "env": env, "deps": deps}) + "\n")
sys.exit(r.returncode)
"""


def wrapper(path, tool, real, watch=False):
    path.write_text(WRAPPER % (tool, str(real), watch, SCRIPT_ENV))
    path.chmod(0o755)


def run_make(src, work, target, variables, log, jobs, env, version: str, darwin: str):
    command = [
        'make',
        '-C',
        str(src),
        target,
        'RC_DARWIN_KERNEL_VERSION=' + darwin,
        'RC_ProjectSourceVersion=' + version,
        'MAKEJOBS=-j' + str(jobs),
        'VERBOSE=YES',
        '-k',
        f'OBJROOT={work}/obj',
        f'SYMROOT={work}/sym',
        f'DSTROOT={work}/dst',
    ] + variables
    with open(work / 'make.log', 'w') as out:
        result = subprocess.run(command, env=env, stdout=out, stderr=subprocess.STDOUT)
    return result.returncode


class Roots:
    def __init__(self, pairs):
        # Longest first: {d} lies inside the work directory, {o} too.
        self.pairs = sorted(pairs, key=lambda p: -len(p[0]))

    def place(self, path, cwd=None):
        """(root, relpath) for a path, or None for one outside every root (/dev/null, /usr/bin/...)."""
        p = path if path.startswith('/') or cwd is None else os.path.join(cwd, path)
        p = os.path.normpath(p)
        for root, name in self.pairs:
            if p == root:
                return name, ''
            if p.startswith(root + '/'):
                return name, p[len(root) + 1 :]
        return None

    def spell(self, path, cwd=None):
        found = self.place(path, cwd)
        if not found:
            return path
        return '{%s}/%s' % found if found[1] else '{%s}' % found[0]

    def text(self, value):
        """Every root path inside a string (an argument like -I/abs/dir) as its placeholder."""
        for root, name in self.pairs:
            value = value.replace(root, '{%s}' % name)
        return value.replace('}//', '}/')


def rel(spelled):
    """'{o}/a/b' -> 'o/a/b' (the table's path key)."""
    m = re.match(r'^\{(\w)\}/(.*)$', spelled)
    if not m:
        raise ValueError('not a rooted path: ' + spelled)
    return m.group(1) + '/' + os.path.normpath(m.group(2))


SYSCALL_MODES = {
    'names': 'syscalls.c',
    'proto': 'sysproto.h',
    'header': 'syscall.h',
    'table': 'init_sysent.c',
    'audit': 'audit_kevents.c',
    'systrace': 'systrace_args.c',
}


def script_outputs(r):
    """The files a script writes (absolute): from its arguments, doconf's from what appeared in its -d directory."""
    tool, argv, cwd = r['tool'], r['argv'], r['cwd']
    if tool.endswith('doconf'):
        return r['written']
    if tool.endswith('makesyscalls.sh'):
        return [os.path.join(cwd, SYSCALL_MODES[argv[1]])] if len(argv) > 1 and argv[1] in SYSCALL_MODES else []
    if tool.endswith('newvers.pl'):
        return [os.path.join(cwd, argv[0])]
    return [os.path.join(cwd, argv[-1])]  # make_posix_availability.sh OUT, make_symbol_aliasing.sh SDK OUT


def script_inputs(r, cwd, roots, outs):
    """The files a script reads: those its arguments and environment name, doconf's MASTER files and conf/
    directory, newvers.pl's $OBJROOT/xnuVersion.
    """
    ins = set()
    for value in list(r['argv']) + list(r['env'].values()):
        found = roots.place(value, cwd)
        if found and found[1] and os.path.isfile(os.path.join(cwd, value)) and found[0] != 'x':
            ins.add(found[0] + '/' + os.path.normpath(found[1]))
    argv = r['argv']
    if r['tool'].endswith('doconf'):
        for flag, pattern in (('-m', 'MASTER*'), ('-s', '*')):
            directory = Path(argv[argv.index(flag) + 1])
            for f in sorted(directory.glob(pattern)):
                if f.is_file():
                    ins.add(rel(roots.spell(str(f))))
    if r['tool'].endswith('newvers.pl'):
        ins.add(rel(roots.spell(os.path.join(r['env']['OBJROOT'], 'xnuVersion'))))
    return sorted(ins - set(outs))


def outputs_of(argv, flags, cwd, roots):
    outs = []
    for i, a in enumerate(argv[:-1]):
        if a in flags and argv[i + 1] != '/dev/null':
            outs.append(rel(roots.spell(argv[i + 1], cwd)))
    return outs


def include_dirs(argv, cwd, roots):
    dirs = []
    for i, a in enumerate(argv):
        d = None
        if a.startswith('-I') and len(a) > 2:
            d = a[2:]
        elif a in ('-I', '-isysroot') and i + 1 < len(argv):
            d = argv[i + 1]
        if d is not None:
            spelled = roots.spell(d, cwd)
            if spelled.startswith('{') and not spelled.startswith('{x}'):
                dirs.append(rel(spelled) if '/' in spelled else spelled.strip('{}') + '/')
    return sorted(set(dirs))


def record_pass(name, src, work, roots, final_files, log_path, make_log):
    """The table entries of one pass from its wrapper log."""
    records = [json.loads(line) for line in log_path.read_text().splitlines()]
    steps, problems = [], []
    unifdef_by_shell = {}
    for r in records:
        if r['tool'] == 'unifdef':
            unifdef_by_shell.setdefault(r['ppid'], []).append(r)
    produced = {}

    def claim(path, step):
        if step.get('duplicate'):
            return
        if path in produced and produced[path] is not step and produced[path] == step:
            step['duplicate'] = True
            return
        if path in produced and produced[path] is not step:
            problems.append('{} written twice ({} and {})'.format(path, produced[path]['kind'], step['kind']))
        produced[path] = step

    used_unifdef = set()
    for r in records:
        cwd = r['cwd']
        if r['tool'] == 'installfile':
            argv = r['argv']
            mode = ' '.join(argv[:-2])
            src_arg, dst_arg = argv[-2], argv[-1]
            dst = rel(roots.spell(dst_arg, cwd))
            m = re.search(r'\.unifdef\.(\d+)$', src_arg)
            if m:
                shell = int(m.group(1))
                stem = os.path.basename(src_arg)[: -len(m.group(0))]
                candidates = [
                    u
                    for u in unifdef_by_shell.get(shell, [])
                    if os.path.basename(u['argv'][-1]) == stem and id(u) not in used_unifdef
                ]
                if len(candidates) != 1:
                    problems.append(f'{dst}: no unique unifdef for {src_arg} ({len(candidates)} found)')
                    continue
                u = candidates[0]
                used_unifdef.add(id(u))
                step = {
                    'kind': 'unifdef',
                    'flags': [roots.text(a) for a in u['argv'][:-1]],
                    'src': rel(roots.spell(u['argv'][-1], u['cwd'])),
                    'out': dst,
                    'mode': mode,
                }
            else:
                step = {'kind': 'copy', 'src': rel(roots.spell(src_arg, cwd)), 'out': dst, 'mode': mode}
            claim(dst, step)
            steps.append(step)
        elif r['tool'] in ('mig', 'iig'):
            argv = r['argv']
            if r['tool'] == 'mig':
                outs = outputs_of(argv, ('-header', '-user', '-server', '-sheader', '-iheader', '-dheader'), cwd, roots)
                source = rel(roots.spell(argv[-1], cwd))
            else:
                outs = outputs_of(argv, ('--header', '--impl'), cwd, roots)
                source = rel(roots.spell(argv[argv.index('--def') + 1], cwd))
            if r['deps'] is None:
                problems.append('{}: no dependency list for {}'.format(r['tool'], source))
                continue
            # What it read besides sources (generated headers: their steps run first); sources come from the
            # include directories (incdirs), pruned at run time.
            generated = sorted({rel(roots.spell(d, cwd)) for d in r['deps'] if roots.place(d, cwd)} - {source})
            step = {
                'kind': r['tool'],
                'cwd': rel(roots.spell(cwd)),
                'argv': [roots.text(a) for a in argv],
                'src': source,
                'outs': outs,
                'incdirs': sorted(set(include_dirs(argv, cwd, roots)) | {os.path.dirname(source)}),
                'deps': [d for d in generated if not d.startswith(('s/', 'x/'))],
            }
            # iokit/Tests/TestServices wraps iig's implementation in #if DEBUG || DEVELOPMENT (echo, cat, echo).
            for o in list(outs):
                if o.endswith('.iig.cpp.tmp') and o[2:-4] in final_files.get(o[0], set()):
                    step['wrapped'] = o[:-4]
                    outs.append(o[:-4])
            for o in outs:
                claim(o, step)
            steps.append(step)
        elif r['tool'] in SCRIPTS:
            # NOT_OURS names what r18-xnu-objs' rules make in the object root; everything in a DSTROOT is ours.
            outs = [rel(roots.spell(p)) for p in script_outputs(r) if roots.place(p, cwd)]
            outs = [o for o in outs if o.startswith('d/') or not NOT_OURS.search(o[2:])]
            outs = [o for o in outs if o[2:] in final_files.get(o[0], set())]
            if not outs:
                continue  # validate_blacklist and the like: nothing kept
            step = {
                'kind': 'script',
                'tool': 's/' + r['tool'],
                'cwd': rel(roots.spell(cwd)),
                'argv': [roots.text(a) for a in r['argv']],
                'env': {k: roots.text(v) for k, v in sorted(r['env'].items())},
                'outs': outs,
                'ins': script_inputs(r, cwd, roots, outs),
            }
            # A script that rewrites a file a copy installed (newvers.pl on version.h.template's copy): one step.
            for o in outs:
                earlier = produced.get(o)
                if earlier is not None and earlier['kind'] == 'copy':
                    step['pre_copy'] = earlier['src']
                    steps.remove(earlier)
                    del produced[o]
            if 'pre_copy' not in step and len(outs) == 1 and os.path.basename(outs[0]) in r['argv']:
                # Rewritten in place after a plain install(1) make does not route through $(INSTALL).
                m = re.search(
                    r'^install -c(?: -S)? -m \d+ (\S+) ' + re.escape(os.path.basename(outs[0])) + '$',
                    make_log.read_text(errors='replace'),
                    re.MULTILINE,
                )
                if m:
                    step['pre_copy'] = rel(roots.spell(m.group(1), cwd))
            for o in outs:
                claim(o, step)
            steps.append(step)
    # unifdef straight into a file (syscalls.master -> bsd.syscalls.master): from make's log, which prints it whole.
    log_text = make_log.read_text(errors='replace')
    unpaired = {}
    for r in records:
        if r['tool'] == 'unifdef' and id(r) not in used_unifdef:
            unpaired.setdefault(r['argv'][-1], []).append(r)
    for source, rs in sorted(unpaired.items()):
        dsts = []
        for m in re.finditer(re.escape(source) + r' > (\S+);', log_text):
            if m.group(1) not in dsts:
                dsts.append(m.group(1))
        flags = {tuple(r['argv'][:-1]) for r in rs}
        for d in dsts:
            dst = rel(roots.spell(d, rs[0]['cwd']))
            if dst[2:] not in final_files.get(dst[0], set()):
                continue
            if len(flags) != 1:
                problems.append(f'{dst}: unifdef runs on it with different flags')
                continue
            step = {
                'kind': 'unifdef_to',
                'flags': [roots.text(a) for a in rs[0]['argv'][:-1]],
                'src': rel(roots.spell(source, rs[0]['cwd'])),
                'out': dst,
            }
            claim(dst, step)
            steps.append(step)
    # Symbolic links (Kernel.framework's Headers, PrivateHeaders, Resources, Versions/Current).
    for root, kind in (('d', 'dst'), ('o', 'obj')):
        base = work / kind
        for f in sorted(final_files.get(root + 'L', set())):
            target = os.readlink(base / f)
            step = {'kind': 'symlink', 'out': root + '/' + f, 'target': target}
            claim(step['out'], step)
            steps.append(step)
    # xnuVersion: make echoes it (no tool to wrap); a pure function of RC_DARWIN_KERNEL_VERSION.
    if 'xnuVersion' in final_files.get('o', set()):
        step = {'kind': 'write', 'out': 'o/xnuVersion', 'content': (work / 'obj/xnuVersion').read_text()}
        claim(step['out'], step)
        steps.append(step)
    steps = [s for s in steps if not s.pop('duplicate', False)]
    return steps, produced, problems


def check_coverage(produced, coverage, final_files):
    problems = ['no step writes ' + p for p in sorted(coverage - set(produced))]
    for p in sorted(produced):
        if p[2:] not in final_files.get(p[0], set()) | final_files.get(p[0] + 'L', set()):
            problems.append(f'a step writes {p} but make left no such file')
    return problems


def listing(root):
    files, links = set(), set()
    if not root.exists():
        return files, links
    for path in root.rglob('*'):
        r = str(path.relative_to(root))
        if path.is_symlink():
            links.add(r)
        elif path.is_file():
            files.add(r)
    return files, links


def write_table(path, name, version, data):
    """A .bzl file: NAME = {"flags": [unifdef flag lists], "steps": [one dict per step]}, sorted, so a re-recorded
    table diffs line by line. A step's "flags" is an index into "flags" (a few flag sets serve thousands of headers).
    """
    flags = sorted({tuple(s['flags']) for s in data if 'flags' in s})
    index = {f: i for i, f in enumerate(flags)}
    lines = [
        f'"""Generated by kernel/rules/xnu_record.py (bazel run //kernel/xnu-{version}:update_tables). Do not edit.',
        '',
        f"{name}: what XNU's make does in this pass besides compiling, one entry per step (kernel/rules/xnu.bzl).",
        '"""',
        '',
        name.upper() + ' = {',
        '    "flags": [',
    ]
    for f in flags:
        lines.append('        ' + json.dumps(list(f)) + ',')
    lines += ['    ],', '    "steps": [']
    for step in sorted(data, key=lambda s: (s['kind'], s.get('out') or s['outs'][0])):
        if 'flags' in step:
            step = dict(step, flags=index[tuple(step['flags'])])
        lines.append('        ' + json.dumps(step, sort_keys=True) + ',')
    lines += ['    ],', '}']
    text = '\n'.join(lines) + '\n'
    text = text.replace(': true', ': True').replace(': false', ': False').replace(': null', ': None')
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('xnu', 'version', 'out', 'availability', 'libplatform', 'libdispatch'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--darwin', required=True, help='the Darwin version of the OS this XNU shipped in')
    parser.add_argument('--ios-min', required=True, help='the iOS deployment target the kernel is built for')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--keep', help='keep the scratch directory here (for debugging)')
    parser.add_argument(
        '--configs',
        default='DEVELOPMENT,RELEASE',
        help="the build passes' kernel configurations (empty: only installhdrs)",
    )
    parser.add_argument('--check', action='store_true', help='exit 1 if the tables in --out differ')
    parser.add_argument('--reuse', action='store_true', help='with --keep: parse the passes already recorded there')
    args = parser.parse_args()
    for name in ('availability', 'libplatform', 'libdispatch'):
        given = Path(getattr(args, name)).resolve()
        setattr(args, name, str(given if given.is_dir() else given.parent))
    workspace = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY', '.'))
    xnu = (workspace / args.xnu).resolve() if not os.path.isabs(args.xnu) else Path(args.xnu)
    out = (workspace / args.out) if not os.path.isabs(args.out) else Path(args.out)
    scratch = Path(args.keep if args.keep else tempfile.mkdtemp(prefix='xnu-record.')).resolve()
    reuse = args.reuse and (scratch / 'src').exists()
    if not reuse:
        shutil.rmtree(scratch, ignore_errors=True)
        scratch.mkdir(parents=True)
    args.reuse = reuse
    tables = {}
    problems = []
    try:
        src = scratch / 'src'
        bin_dir = scratch / 'bin'
        if not reuse:
            shutil.copytree(xnu, src, ignore=shutil.ignore_patterns('BUILD.bazel', '*.bzl', 'tables'))
            bin_dir.mkdir()
        for tool in ('mig', 'iig', 'unifdef'):
            wrapper(bin_dir / tool, tool, TOOLCHAIN + '/' + tool)
        for script in SCRIPTS:
            real = src / (script + '.real')
            if not reuse:
                (src / script).rename(real)
            wrapper(src / script, script, real, watch=True)
        env = {
            'PATH': '/usr/bin:/bin:/usr/sbin:/sbin',
            'DEVELOPER_DIR': DEVELOPER_DIR,
            'HOME': str(scratch),
            'TMPDIR': str(scratch),
            'LC_ALL': 'en_IE.UTF-8',
            'LANG': 'en_IE.UTF-8',
            'TZ': 'UTC',
        }
        tools = [f'MIG={bin_dir}/mig', f'IIG={bin_dir}/iig', f'UNIFDEF={bin_dir}/unifdef']
        # --- installhdrs: the SDK root, its other parts first (as sdkroot.py) ---
        work = scratch / 'installhdrs'
        dst = work / 'dst'
        base_dir = work / 'base'
        if not reuse:
            base_dir.mkdir(parents=True)
            xnu_ops.install_base(
                Path(args.availability).resolve(),
                Path(args.libplatform).resolve(),
                Path(args.libdispatch).resolve(),
                base_dir,
                env,
            )
            shutil.copytree(base_dir, dst, symlinks=True)
        base_files = set(listing(base_dir)[0])
        install_tool = bin_dir / 'installfile-ih'
        ih_env = dict(env, KERNEL_SDKROOT=str(dst), XNU_AVAILABILITY_PL=str(dst / 'usr/local/libexec/availability.pl'))
        # installfile is built by make's setup; the wrapper calls the copy make built.
        wrapper(install_tool, 'installfile', work / 'obj/SETUP/installfile/installfile')
        log = work / 'tools.jsonl'
        if not (args.reuse and log.exists()):
            log.write_text('')
            run_make(
                src,
                work,
                'installhdrs',
                INSTALLHDRS_VARS + tools + ['INSTALL=' + str(install_tool)],
                log,
                args.jobs,
                dict(ih_env, XNU_RECORD_LOG=str(log)),
                args.version,
                args.darwin,
            )
        roots = Roots([(str(src), 's'), (str(work / 'obj'), 'o'), (str(dst), 'd'), (DEVELOPER_DIR, 'x')])
        files, links = listing(dst)
        o_files, _ = listing(work / 'obj')
        final = {'d': files, 'dL': links, 'o': o_files}
        steps, produced, found = record_pass('installhdrs', src, work, roots, final, log, work / 'make.log')
        # The SDK root's other parts (availability.pl and its headers, libplatform, libdispatch os/): one step, the
        # files installhdrs did not write over.
        base_step = {'kind': 'base', 'outs': sorted('d/' + f for f in base_files if 'd/' + f not in produced)}
        steps.append(base_step)
        for o in base_step['outs']:
            produced[o] = base_step
        found += check_coverage(produced, {'d/' + f for f in files | links}, final)
        tables['installhdrs'] = steps
        problems += ['installhdrs: ' + p for p in found]
        sdkroot = dst
        # --- build, per kernel configuration ---
        for config in [c for c in args.configs.split(',') if c]:
            work = scratch / ('build-' + config)
            work.mkdir(exist_ok=True)
            install_tool = bin_dir / ('installfile-' + config)
            wrapper(install_tool, 'installfile', work / 'obj/SETUP/installfile/installfile')
            log = work / 'tools.jsonl'
            b_env = dict(
                env,
                KERNEL_SDKROOT=str(sdkroot),
                XNU_AVAILABILITY_PL=str(sdkroot / 'usr/local/libexec/availability.pl'),
                KERNEL_BUILD_DATE='Thu Jan  1 00:00:00 UTC 1970',
            )
            if not (args.reuse and log.exists()):
                log.write_text('')
                run_make(
                    src,
                    work,
                    'build',
                    build_vars(args.ios_min) + ['KERNEL_CONFIGS=' + config] + tools + ['INSTALL=' + str(install_tool)],
                    log,
                    args.jobs,
                    dict(b_env, XNU_RECORD_LOG=str(log)),
                    args.version,
                    args.darwin,
                )
            roots = Roots([(str(src), 's'), (str(work / 'obj'), 'o'), (str(sdkroot), 'k'), (DEVELOPER_DIR, 'x')])
            files, links = listing(work / 'obj')
            final = {'o': files, 'oL': links}
            coverage = {'o/' + f for f in files if not NOT_OURS.search(f)}
            steps, produced, found = record_pass('build_' + config, src, work, roots, final, log, work / 'make.log')
            found += check_coverage(produced, coverage, final)
            tables['build_' + config] = steps
            problems += [f'build {config}: {p}' for p in found]
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)
    if problems:
        sys.exit('\n'.join(problems[:200]) + (f'\n... {len(problems)} problems' if len(problems) > 200 else ''))
    out.mkdir(parents=True, exist_ok=True)
    if args.check:
        with tempfile.TemporaryDirectory() as fresh:
            stale = []
            for name, data in tables.items():
                write_table(Path(fresh) / (name + '.bzl'), name, args.version, data)
                if (Path(fresh) / (name + '.bzl')).read_text() != (out / (name + '.bzl')).read_text():
                    stale.append(name)
            if stale:
                sys.exit(
                    'stale tables: {} (bazel run //kernel/xnu-{}:update_tables)'.format(', '.join(stale), args.version)
                )
        print('tables current')
        return
    for name, data in tables.items():
        write_table(out / (name + '.bzl'), name, args.version, data)
        print('{}: {} steps'.format(out / (name + '.bzl'), len(data)))


if __name__ == '__main__':
    main()
