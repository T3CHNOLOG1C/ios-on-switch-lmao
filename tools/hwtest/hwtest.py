#!/usr/bin/env python3
"""HW Test's runs against the runs before them (README.md).

    bazel run //tools/hwtest -- [--history DIR] [REPORT.json ...]

With no report named, the newest run of each app and iOS build in the history. pull_logs fetches the card's reports
into the history and prints this for the runs it found new.
"""

import argparse
import os
import sys
from collections.abc import Callable
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.hwtest import compare, fetch
from tools.hwtest.reports import Report, history

SUMMARY = 'hwtest-summary.txt'


def history_dir() -> Path:
    return paths.state_dir() / 'device-logs' / 'hwtest'


def summaries(reports: list[Report], chosen: list[Report]) -> str:
    """Each chosen report against the earlier runs of its app on its build."""
    texts = []
    for latest in chosen:
        earlier = [r for r in reports if r.key == latest.key and r.started < latest.started]
        texts.append(compare.summary(latest, earlier))
    return '\n\n'.join(texts)


def newest_per_key(reports: list[Report]) -> list[Report]:
    newest: dict[tuple[str, str], Report] = {}
    for report in reports:
        newest[report.key] = report
    return list(newest.values())


def after_pull(data: Path, switch_folder: Path, out: Path, log: Callable[..., None]) -> None:
    """Fetch the card's reports into the history and `out`, and log the summary of the newest new run of each app."""
    archive = out / 'boot.logarchive'
    changed = {path.name for path in fetch.fetch(data, switch_folder, history_dir(), out, archive)}
    if not changed:
        log('hwtest: no new HW Test run on the card')
        return
    reports = history(history_dir())
    text = summaries(reports, newest_per_key([r for r in reports if r.path.name in changed]))
    (out / SUMMARY).write_text(text + '\n')
    log(f'hwtest: {len(changed)} new or updated run reports -> {history_dir()}\n{text}')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reports', nargs='*', type=Path)
    parser.add_argument('--history', type=Path, default=None)
    args = parser.parse_args(argv)
    reports = history(args.history or history_dir())
    held = {r.path.resolve() for r in reports}
    reports = sorted(
        reports + [Report.load(path) for path in args.reports if path.resolve() not in held], key=lambda r: r.started
    )
    named = {path.resolve() for path in args.reports}
    chosen = [r for r in reports if r.path.resolve() in named] if named else newest_per_key(reports)
    if not chosen:
        print('no HW Test reports in', args.history or history_dir())
        return 1
    print(summaries(reports, chosen))
    return 0


if __name__ == '__main__':
    sys.exit(main())
