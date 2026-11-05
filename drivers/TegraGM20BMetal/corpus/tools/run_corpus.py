#!/usr/bin/env python3
"""Run every metallib of the corpus through m2v-corpus (host NVK over the mock GM20B), one process per library, resuming after a
crash or a hang at the next function. Writes <out>/<tag>.tsv (rows) and <out>/<tag>.log (stderr tail per crash).

usage: run_corpus.py <corpus.tsv> <out dir> [--jobs N] [--only substr] [--timeout s]
corpus.tsv: tag<TAB>path per line (tools/make_corpus.py).
Environment: RUNNER (corpus/runner's m2v-corpus: bazel build of its BUILD.bazel's :m2v-corpus), NVK (a host build of libvulkan_nouveau.dylib: scratchpad/gpu/userland/build-host,
or build-host-rel for the iOS build's release NAK), MOCK (hostsim's libmock_iokit.dylib), RUNNER_ARGS (--draw: also draw each
render pipeline three times in one pass)."""
import os, subprocess, sys, shutil, concurrent.futures as cf
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
S = BASE.parent
RUNNER = Path(os.environ.get('RUNNER', S.parents[1] / 'bazel-bin/drivers/TegraGM20BMetal/corpus/runner/m2v-corpus'))
NVK = Path(os.environ.get('NVK', S / 'gpu/userland/build-host/src/nouveau/vulkan/libvulkan_nouveau.dylib'))
MOCK = Path(os.environ.get('MOCK', S / 'gpu/hostsim/libmock_iokit.dylib'))

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def run_one(tag, path, out, timeout):
    safe = tag.replace('/', '_')
    rows, log = out / (safe + '.tsv'), out / (safe + '.log')
    cache = out / 'cache' / safe
    rows.write_text(''); log.write_text('')
    start, crashes = 0, 0
    env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(MOCK), METAL2VULKAN_LOG_PIPELINES='1', MESA_SHADER_CACHE_DISABLE='true')
    while True:
        cache.mkdir(parents=True, exist_ok=True)
        cmd = [str(RUNNER), str(NVK), str(cache), path, '--tag', tag, '--from', str(start)] + os.environ.get('RUNNER_ARGS', '').split()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=timeout)
            stdout, stderr, code, hung = p.stdout, p.stderr, p.returncode, False
        except subprocess.TimeoutExpired as e:
            stdout = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            stderr = (e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else (e.stderr or '')
            code, hung = -9, True
        with rows.open('a') as f:
            f.write(stdout)
        lines = stderr.splitlines()
        if lines and lines[-1] == 'END' and code == 0:
            break
        begins = [l for l in lines if l.startswith('BEGIN ')]
        if not begins:
            with rows.open('a') as f:
                f.write('%s\t-\t-\t-\tERR\t%s\t%s\t0\n' % (tag, 'hang' if hung else 'crash',
                        ' | '.join(l for l in lines[-6:] if 'NVK is not a conformant' not in l)[:1500].replace('\t', ' ')))
            break
        _, pos, name = begins[-1].split(' ', 2)
        tail = [l for l in lines[lines.index(begins[-1]) + 1:] if 'NVK is not a conformant' not in l][-8:]
        why = ('hang > %ds' % timeout) if hung else ('signal %d' % -code if code < 0 else 'exit %d' % code)
        with rows.open('a') as f:
            f.write('%s\t%s\t?\t?\tERR\t%s\t%s: %s\t0\n' % (tag, name, 'hang' if hung else 'crash', why,
                    ' | '.join(tail)[:1500].replace('\t', ' ')))
        with log.open('a') as f:
            f.write('== %s %s (%s)\n%s\n' % (pos, name, why, '\n'.join(tail)))
        crashes += 1
        start = int(pos) + 1
        if crashes > 400:
            break
    shutil.rmtree(cache, ignore_errors=True)
    return tag, crashes

def main():
    corpus, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    jobs, only, timeout = int(opt('--jobs', '6')), opt('--only', None), int(opt('--timeout', '900'))
    entries = [l.split('\t') for l in corpus.read_text().splitlines() if l and not l.startswith('#')]
    entries = [(t, p) for t, p in entries if not only or any(o in t for o in only.split(','))]
    # biggest first so the long ones start early
    entries.sort(key=lambda e: -Path(e[1]).stat().st_size)
    with cf.ThreadPoolExecutor(jobs) as pool:
        for tag, crashes in pool.map(lambda e: run_one(e[0], e[1], out, timeout), entries):
            print('done', tag, 'crashes', crashes, flush=True)

if __name__ == '__main__':
    main()
