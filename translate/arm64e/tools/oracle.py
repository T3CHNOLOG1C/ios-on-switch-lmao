"""The classifier and translated output against LLVM's disassembler (a developer check, not a Bazel test).

    bazel run //translate/arm64e:oracle -- features [SAMPLES]     per feature: words that need it (LLVM decodes them
                                                                    for apple-m4 only with the feature on), and what
                                                                    a64x calls them: misses and false positives
    bazel run //translate/arm64e:oracle -- a57 WORDS.txt          which words (first column, hex) a Cortex-A57 cannot
                                                                    decode, by a64x class (arm64e xlate -d writes them)

LLVM_MC names llvm-mc (default: llvm-mc on PATH); ARM64E names the CLI (default: the one bazel run puts beside this).
"""

import collections
import os
import random
import re
import subprocess
import sys

LLVM_MC = os.environ.get('LLVM_MC', 'llvm-mc')
CHUNK = 200000

# feature -> (a64x class names that count as that feature, top bytes where its encodings live)
FEATURES = {
    'fullfp16': (
        ('fp16', 'fhm', 'fcma'),
        (0x0E, 0x2E, 0x4E, 0x6E, 0x5E, 0x7E, 0x1E, 0x9E, 0x0F, 0x2F, 0x4F, 0x6F, 0x5F, 0x7F),
    ),
    'rdm': (('rdm',), (0x2E, 0x6E, 0x7E, 0x2F, 0x6F, 0x7F)),
    'jsconv': (('jscvt',), (0x1E,)),
    'complxnum': (('fcma',), (0x2E, 0x6E, 0x2F, 0x6F)),
    'lse': (('lse.cas', 'lse.casp', 'lse.ldop', 'lse.swp'), (0x08, 0x48, 0x88, 0xC8, 0x38, 0x78, 0xB8, 0xF8)),
    'pauth': (
        ('pac.br', 'pac.blr', 'pac.ret', 'pac.pac_aut', 'pac.xpac', 'pac.pacga', 'pac.ldra'),
        (0xDA, 0xD6, 0xD7, 0xF8, 0x9A),
    ),
}


def arm64e_cli() -> str:
    if 'ARM64E' in os.environ:
        return os.environ['ARM64E']
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(here, '..', 'arm64e'), os.path.join(here, 'arm64e')):
        if os.path.exists(candidate):
            return candidate
    return 'arm64e'


def invalid(words: list[int], args: list[str]) -> set[int]:
    """Return the indices of the words LLVM cannot decode with these options."""
    bad = set()
    for base in range(0, len(words), CHUNK):
        chunk = words[base : base + CHUNK]
        text = ''.join(
            f'0x{w & 255:02x} 0x{(w >> 8) & 255:02x} 0x{(w >> 16) & 255:02x} 0x{w >> 24:02x}\n' for w in chunk
        )
        p = subprocess.run(
            [LLVM_MC, '--disassemble', '-triple=aarch64', *args], input=text.encode(), capture_output=True, check=False
        )
        for m in re.finditer(rb'<stdin>:(\d+):\d+: warning: invalid instruction encoding', p.stderr):
            bad.add(base + int(m.group(1)) - 1)
    return bad


def classes(words: list[int]) -> list[str]:
    out = subprocess.run(
        [arm64e_cli(), 'class'], input=''.join(f'{w:08x}\n' for w in words).encode(), capture_output=True, check=True
    )
    return out.stdout.decode().split()


def features(samples: int) -> None:
    random.seed(11)
    for feat, (names, tops) in FEATURES.items():
        words = [(t << 24) | random.getrandbits(24) for t in tops for _ in range(samples)]
        bad_with = invalid(words, ['-mcpu=apple-m4'])
        bad_without = invalid(words, ['-mcpu=apple-m4', '-mattr=-' + feat])
        cls = classes(words)
        need = [i for i in range(len(words)) if i not in bad_with and i in bad_without]
        missed = collections.Counter(cls[i] for i in need if cls[i] not in names)
        flagged = [i for i in range(len(words)) if cls[i] in names]
        unallocated = sum(1 for i in flagged if i in bad_with)
        other = sum(1 for i in flagged if i not in bad_with and i not in bad_without)
        print(
            f'{feat:<10} {len(words):8d} sampled, {len(need):7d} need it; missed {dict(missed) or 0}; '
            f'a64x flags {len(flagged)}: {unallocated} unallocated, {other} valid without it'
        )


def a57(path: str) -> None:
    with open(path) as lines:
        words = [int(line.split()[0], 16) for line in lines if line.strip()]
    bad = sorted(invalid(words, ['-mcpu=cortex-a57']))
    tally = collections.Counter()
    if bad:
        cls = classes([words[i] for i in bad])
        tally.update(cls)
    print(f'{len(words)} words, {len(bad)} a Cortex-A57 cannot decode')
    for name, n in tally.most_common():
        print(f'  {name:<16} {n}')


def main() -> None:
    if len(sys.argv) >= 2 and sys.argv[1] == 'features':
        features(int(sys.argv[2]) if len(sys.argv) > 2 else 100000)
    elif len(sys.argv) == 3 and sys.argv[1] == 'a57':
        a57(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
