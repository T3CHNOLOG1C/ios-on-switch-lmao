"""A corpus line changes only its program's bucket: every other bucket keeps its bytes, so Bazel keeps its compile."""

import unittest

from tools.gpu.shaders import plan

LIB = '/System/Library/Frameworks/X.framework/default.metallib'


def _render(vertex: str, fragment: str, target: str = 'samples=1 topo=0 ds=0/0 c0=80:0:0:1:0:0:1:0:0') -> str:
    return f'R ok=1 label=- {target} | v {vertex} lib={LIB} | f {fragment} lib={LIB} | '


CORPUS = [_render(f'v{i}', f'f{i}') for i in range(40)] + [
    f'S ok=1 lib={LIB} name=f{i} k=#0:53:0{i % 2} | ' for i in range(20)
]


def _buckets(corpus: list[str], count: int = 16) -> list[list[str]]:
    return plan.bucket(plan.lines_for('corpus', corpus, {}), count)[0]


class BucketTest(unittest.TestCase):
    def test_an_appended_line_rewrites_only_its_programs_bucket(self):
        before = _buckets(CORPUS)
        line = _render('v3', 'f3', 'samples=4 topo=0 ds=0/0 c0=80:0:0:1:0:0:1:0:0')
        after = _buckets([*CORPUS, line])
        changed = [i for i, (old, new) in enumerate(zip(before, after, strict=True)) if old != new]
        self.assertEqual(len(changed), 1)
        self.assertIn(line, after[changed[0]])
        self.assertIn(_render('v3', 'f3'), after[changed[0]], 'a program keeps its pipelines in one bucket')

    def test_the_order_of_the_corpus_does_not_move_lines_between_buckets(self):
        self.assertEqual([sorted(b) for b in _buckets(CORPUS)], [sorted(b) for b in _buckets(list(reversed(CORPUS)))])

    def test_a_repeated_line_is_made_once(self):
        self.assertEqual(_buckets(CORPUS), _buckets([*CORPUS, CORPUS[0], CORPUS[-1]]))


if __name__ == '__main__':
    unittest.main()
