"""Boot to a milestone on the emulator and fail on the way there: a gate for the batch builds.

    bazel run //tools/emu:check -- --kind=ios17|ios27 [--until launchd] [--timeout 600] [boot.py's options]

Exits 0 once the kernel log shows the milestone, 1 on a panic (its line printed), 2 when the time runs out first (the
last kernel lines printed). The run directory is kept as boot.py writes it.
"""

import argparse
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from tools.emu import boot, msgbuf, report
from tools.emu.monitor import Monitor

MILESTONES = {
    'cores': r'ArmCpu: 3 of 3 secondary cores running',
    'display': r'SwitchDisplay: vsync from the DC A vblank interrupt',
    'sd': r'SwitchSD: ADMA2 on',
    'root': r'BSD root: ',
    'launchd': r'requested by: init \(pid 1\)',
}
PANIC_TEXT = (report.RAMOOPS[0] + 0x180000, 0x80000)


@dataclass
class Watch:
    run: Path
    kernel: Path | None
    milestone: re.Pattern
    panic: str | None = None
    reached: str | None = None
    log: list[str] = field(default_factory=list)

    def __call__(self, monitor: Monitor) -> bool:
        text = self.run / 'panic-text.raw'
        monitor.command('pmemsave', val=PANIC_TEXT[0], size=PANIC_TEXT[1], filename=str(text))
        self.panic = report.panic_text(text.read_bytes())
        text.unlink()
        if self.kernel:
            live = msgbuf.read(monitor, self.kernel, monitor.human('info registers -a'))
            if live:
                self.log = report.kernel_log(live)
        self.reached = next((line for line in self.log if self.milestone.search(line)), None)
        return bool(self.panic or self.reached)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    boot.add_arguments(parser)
    parser.add_argument('--until', choices=sorted(MILESTONES), default='launchd')
    parser.add_argument('--timeout', type=float, default=600, help='wall seconds before giving up')
    args = parser.parse_args()
    run, inputs = boot.prepare(args)
    if not inputs.kernel:
        sys.exit('check needs the prelinked kernel for the live log: --kernel with --package')
    watch = Watch(run, inputs.kernel, re.compile(MILESTONES[args.until]))
    started = time.monotonic()
    boot.run_machine(run, inputs, args.timeout, watch)
    wall = time.monotonic() - started
    report.summarize(run)
    panic = watch.panic or report.panic_text((run / 'ramoops.raw').read_bytes())
    if panic:
        print(f'PANIC after {wall:.0f} s wall ({run}):\n{panic}')
        return 1
    if watch.reached:
        print(f'reached {args.until} after {wall:.0f} s wall: {watch.reached} ({run})')
        return 0
    print(f'TIMEOUT: no {args.until} in {wall:.0f} s wall ({run}); last kernel lines:', *watch.log[-8:], sep='\n')
    return 2


if __name__ == '__main__':
    sys.exit(main())
