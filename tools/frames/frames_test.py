"""tools/frames over lines in the formats the kernel and the GPU user side log."""

import unittest

from tools.frames import frames

KERNEL = (
    'kernel: SwitchFrame: latch 192000000 after 3 frames (50000 us), cause gpu; origin vsync 191040000; '
    'first submit 4000, last submit 6000 (3, exec 300 us), swap 6500, offer 6600, gpu done 31000, '
    'flip 31200, latch 50000 us; other submits 2; swap thread base 47 sched 47 mode 3 qos 6; '
    'cpu 1785 MHz at vsync 1785 at swap'
)
USER = 'backboardd: SwitchFrameUser: backboardd cb 42 commit 191100000 replay 191100100 191200000 fence 0'


class FramesTest(unittest.TestCase):
    def test_stages_and_join(self):
        got = frames.collect([KERNEL, USER])
        self.assertEqual(len(got), 1)
        frame = got[0]
        stages = dict((name, us) for name, us, _ in frame.stages())
        self.assertEqual(stages['vsync->first submit'], 4000)
        self.assertEqual(stages['offer->gpu done'], 24400)
        self.assertEqual(stages['flip->latch'], 18800)
        self.assertEqual(frame.user[0][2], 42)

    def test_missing_points_are_skipped(self):
        line = KERNEL.replace('first submit 4000, last submit 6000', 'first submit -1, last submit -1')
        names = [name for name, _, _ in frames.collect([line])[0].stages()]
        self.assertNotIn('vsync->first submit', names)
        self.assertIn('swap->offer', names)

    def test_summary_names_the_costliest_stage(self):
        out = '\n'.join(frames.summary(frames.collect([KERNEL])))
        self.assertIn('costliest stage: offer->gpu done 1', out)
        self.assertIn('2 refresh periods lost', out)


if __name__ == '__main__':
    unittest.main()
