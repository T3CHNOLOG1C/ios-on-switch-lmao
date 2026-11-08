"""check.py's milestones against kernel log lines from emulator runs."""

import re
import unittest

from tools.emu import check

LAUNCHD = re.compile(check.MILESTONES['launchd'])


class LaunchdMilestoneTest(unittest.TestCase):
    def test_the_kernels_mounts_before_launchd_do_not_count(self):
        for line in (
            (
                '[    17.834832] apfs_log_op_with_proc:3290: disk1s3 mounting volume SwitchPrebootTrial, '
                'requested by: init (pid 1); parent: kernel_task (pid 0)'
            ),
            (
                '[    17.277098] apfs_log_op_with_proc:3290: disk1s1 mounting volume RaveB24B119.J181J182OS, '
                'requested by: kernel_task (pid 0); parent: kernel_task (pid 0)'
            ),
            '[    28.386237] load_init_program: attempting to load /sbin/launchd',
        ):
            self.assertIsNone(LAUNCHD.search(line), line)

    def test_launchd_running_counts(self):
        line = (
            '[    24.113775] shared_region: 0x29a3324b708ef361 [1(launchd)] check_np(0x16ef6af18) '
            'vm_shared_region_start_address() returned 0x1'
        )
        self.assertIsNotNone(LAUNCHD.search(line))

    def test_a_job_launchd_spawned_counts(self):
        line = (
            '[    31.002114] apfs_log_op_with_proc:3290: disk1s2 mounting volume Data, requested by: mount_apfs '
            '(pid 34); parent: launchd (pid 1)'
        )
        self.assertIsNotNone(LAUNCHD.search(line))


if __name__ == '__main__':
    unittest.main()
