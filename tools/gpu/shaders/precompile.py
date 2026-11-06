"""Make one shard of the seed: its lines through the prebuilt precompile tool, into an empty cache directory.

usage: precompile.py --tools <dir> --root <library root> --store <store.json> [--embedded <header>]... --qc <metallib>
                     --lines <shard.log> [--seed <seed dir>] [--guess-dispatch] --out <dir>

<tools> is the host tools' directory (//gpu/seed:tools: precompile, libmock_iokit.dylib beside it, the NVK twin). The
shard's lines after its `# derived` line are guesses: a refusal of one is counted, and a crash in one costs that line
only. The tool is run again past the line it died in (its PRECOMPILE_PROGRESS file says which), on the same cache
directory, so what it made before the crash is read back, not made again. Writes <out>/v1 and <out>/report.json.
--seed: the lines are made over that seed, as the device reads it; report.json's `rates` says how many were found there.
--guess-dispatch: every compute state no D line names is dispatched at the 2D shape Apple's documentation computes.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DERIVED = '# derived'
TOOLS = ('precompile', 'libmock_iokit.dylib', 'libvulkan_nouveau.dylib')
KINDS = 'SRCD'
CRASHES = 64
TIMEOUT = 3600


def store_view(store: Path, root: Path, by_sha: dict[str, str], embedded: list[Path]) -> None:
    """Lay out the library store precompile.mm reads: root/ and data/<sha256>.metallib."""
    (store / 'data').mkdir(parents=True)
    (store / 'root').symlink_to(root.absolute())
    for digest, path in by_sha.items():
        (store / 'data' / (digest + '.metallib')).symlink_to(Path(path).absolute())
    for header in embedded:
        text = header.read_text()
        library = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', text[text.index('{') : text.rindex('}')]))
        (store / 'data' / (hashlib.sha256(library).hexdigest() + '.metallib')).write_bytes(library)


def by_kind(lines: list[str]) -> dict[str, list[str]]:
    """Return the lines of each kind, in the order precompile.mm makes them."""
    return {kind: [line for line in lines if line.startswith(kind + ' ')] for kind in KINDS}


def remaining(corpus: dict[str, list[str]], derived: dict[str, list[str]], kind: str, index: int) -> tuple:
    """Split what is left after the line `index` of `kind` (corpus lines first, as the tool reads them) died."""
    order = KINDS.index(kind)
    left_corpus, left_derived = {}, {}
    for k in KINDS:
        both = corpus[k] + derived[k]
        start = 0 if KINDS.index(k) > order else index + 1 if k == kind else len(both)
        left_corpus[k] = [line for i, line in enumerate(both) if i >= start and i < len(corpus[k])]
        left_derived[k] = [line for i, line in enumerate(both) if i >= start and i >= len(corpus[k])]
    return left_corpus, left_derived


def _journal(path: Path, lines: dict[str, list[str]]) -> Path:
    path.write_text(''.join(line + '\n' for k in KINDS for line in lines[k]))
    return path


RATES = re.compile(r'(cache|drv|pso)=(\S+)')


def rates(summaries: list[str]) -> dict[str, str]:
    """Return the cache, driver and pipeline-state counts of the tool's last rates line."""
    lines = [line for line in summaries if line.startswith('rates:')]
    return dict(RATES.findall(lines[-1])) if lines else {}


def _died_at(progress: Path, corpus: dict, derived: dict, output: str) -> tuple[str, int]:
    """Return the line the tool was making when it died, after checking it read the lines as they were given."""
    words = progress.read_text().split() if progress.exists() else []
    if len(words) < 7 or words[0] != 'lines':
        sys.exit(f'precompile died before its first line:\n{output[-4000:]}')
    given = [len(corpus[k]) + len(derived[k]) for k in KINDS]
    if [int(n) for n in words[1:5]] != given:
        sys.exit(f'precompile read {words[1:5]} lines by kind, not the {given} given (two lines it takes as one)')
    return words[-2], int(words[-1])


def make(work: Path, args: argparse.Namespace, corpus: dict, derived: dict) -> dict:
    """Run the tool until every line is made or skipped; return what happened."""
    env = {
        'PATH': '/usr/bin:/bin',
        'HOME': str(work / 'home'),
        'TMPDIR': str(work / 'tmp'),
        'MESA_SHADER_CACHE_DISABLE': 'true',
        'PRECOMPILE_PROGRESS': str(work / 'progress'),
    }
    if args.seed:
        env['METAL2VULKAN_SHADER_CACHE_SEED'] = str(args.seed.absolute())
    (work / 'home').mkdir()
    (work / 'tmp').mkdir()
    crashed, summaries, refusals = [], [], []
    while any(corpus.values()) or any(derived.values()):
        command = [
            work / 'precompile',
            '--libs',
            work / 'store',
            work / 'libvulkan_nouveau.dylib',
            work / 'cache',
            args.qc,
            _journal(work / 'corpus.log', corpus),
            '--derived',
            _journal(work / 'derived.log', derived),
            *(['--guess-dispatch'] if args.guess_dispatch else []),
        ]
        try:
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=TIMEOUT, check=False)
            died, output = result.returncode < 0, result.stdout + result.stderr
        except subprocess.TimeoutExpired as expired:
            died, output = True, str(expired.stdout or '') + str(expired.stderr or '')
        refusals += [line[8:] for line in output.splitlines() if line.startswith('REFUSED ')]
        summaries += [line for line in output.splitlines() if line.startswith(('precompile:', 'rates:'))]
        if not died:
            if result.returncode:
                sys.exit(f'precompile failed ({result.returncode}):\n{output[-6000:]}')
            break
        kind, index = _died_at(work / 'progress', corpus, derived, output)
        line = (corpus[kind] + derived[kind])[index]
        if index < len(corpus[kind]) or len(crashed) >= CRASHES:
            sys.exit(f'precompile died making {kind} line {index}: {line[:400]}\n{output[-4000:]}')
        crashed.append(line)
        corpus, derived = remaining(corpus, derived, kind, index)
    return {'crashed': crashed, 'refused': refusals, 'runs': summaries, 'rates': rates(summaries)}


def main() -> None:
    """Make the shard into <out>/v1."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('tools', 'root', 'store', 'qc', 'lines', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--embedded', type=Path, action='append', default=[])
    parser.add_argument('--seed', type=Path)
    parser.add_argument('--guess-dispatch', action='store_true')
    args = parser.parse_args()
    text = args.lines.read_text().split(DERIVED + '\n')
    corpus, derived = by_kind(text[0].splitlines()), by_kind(text[1].splitlines() if len(text) > 1 else [])
    work = Path.cwd() / (f'_shard_{args.lines.stem}')
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in TOOLS:
        shutil.copyfile(args.tools / name, work / name)
        (work / name).chmod(0o755)
    store_view(work / 'store', args.root, json.loads(args.store.read_text()), args.embedded)
    began = time.monotonic()
    report = make(work, args, corpus, derived) if any(corpus.values()) or any(derived.values()) else {'runs': []}
    report['seconds'] = round(time.monotonic() - began, 1)
    report['lines'] = sum(map(len, corpus.values())) + sum(map(len, derived.values()))
    shutil.rmtree(args.out, ignore_errors=True)
    args.out.mkdir(parents=True)
    if (work / 'cache' / 'v1').is_dir():
        shutil.copytree(
            work / 'cache' / 'v1', args.out / 'v1', copy_function=shutil.copyfile, ignore=shutil.ignore_patterns('.*')
        )
    (args.out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    shutil.rmtree(work)
    crashed, refused = len(report.get('crashed', [])), len(report.get('refused', []))
    lines, seconds = report['lines'], report['seconds']
    print(f'shard {args.lines.stem}: {lines} lines in {seconds} s, {refused} refused, {crashed} crashed')


if __name__ == '__main__':
    main()
