#!/usr/bin/env python3
"""Pull the last boots' logs off the card's iPad Data volume (read-only but for harvest.py's one documented write).

    bazel run //deploy:pull_logs -- <diskN> [--out DIR] [--no-logarchive] [--no-harvest] [--no-csstore] [--no-launchd]
                                            [--no-eject]

Copies into <out> (default <state>/device-logs/boot-<MMDD-HHMM>, <state> being ~/Library/Application Support/switch-ios or
$SWITCH_HOME: outside every checkout):
  diagnostics/, uuidtext/               the unified log (Data db/), and <out>/boot.logarchive built from them
                                        (`/usr/bin/log show --archive <out>/boot.logarchive --style compact --info --debug`)
  launchd.log                           (--no-launchd skips it)
  mobile-CrashReporter/, *.ips          crash reports, the WebKit ones also at the top level
  mobileactivationd/, migration-state.txt, boot-io-census.bin
  panic-<UTC>.log, .kcdata              the last saved kernel panic (SwitchPanicSave.c: the panic string, kernel log
                                        tail and XNU's panic log; its stackshot), with the stackshot decoded by XNU's
                                        kcdata.py into panic-<UTC>.stackshot.ips (every task's threads, as stacks-*.ips)
                                        and panic-<UTC>.threads.txt (the threads of the process the panic names, e.g.
                                        the one a "userspace watchdog timeout" found hung)
  power.log, power.prev.log            the power journal (SwitchWake.h: every display off/on, wake source, wake
                                        guarantee and system sleep step) of the last boot and the one before
  wake-<UTC>.kcdata                     a stackshot of every task taken when the wake guarantee lit the panel itself (a
                                        wake source with the panel off and no display-on from the render server),
                                        decoded as the panic ones into wake-<UTC>.stackshot.ips and wake-<UTC>.threads.txt
                                        (SpringBoard's and backboardd's threads)
  com.apple.LaunchServices-*.csstore + lsdb-webkit.txt   (--no-csstore skips them)
  m2v/ (+ m2v-cache -> backboardd's)    every process's m2v shader cache, whose journals are merged into the
                                        precompile corpus (tree/drivers/TegraGM20BMetal/precompile/corpus.log, a source
                                        file in your checkout) by that directory's harvest.py (--no-harvest skips it)
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.deploy import mkarchive  # noqa: E402
from tools.install import card as cards  # noqa: E402
from tools.install import host  # noqa: E402
from tools.install import renamed  # noqa: E402

DATA_UUID = 'D1B1D053-D0A6-496C-8625-A947A3DA863E'  # SwitchRawMount.h SWITCH_RAW_DATA_UUID (checked against spec)
# The checkout's precompile directory (harvest.py and the corpus.log it merges into), relative to the workspace: --harvest
# (//deploy's $(rootpath) of harvest.py, so it follows the file if it moves).
HARVEST = None
WEBKIT_IDS = (
    'com.apple.WebKit.WebContent',
    'com.apple.WebKit.Networking',
    'com.apple.WebKit.GPU',
    'com.apple.WebKit.WebContent.CaptivePortal',
    'WebContentExtension.appex',
    'com.apple.fskit.exfat',
)
BACKBOARDD_CACHE = 'm2v/System_6C7A65AF-CB18-421C-A07A-D724B05A7AB6_Library_Caches_m2v'


def state_dir():
    """Where the tools keep what is not the repository's: ~/Library/Application Support/switch-ios, or $SWITCH_HOME."""
    return Path(os.environ.get('SWITCH_HOME') or Path.home() / 'Library/Application Support/switch-ios')


def workspace():
    return Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path(os.path.abspath(__file__)).parents[2])


def copytree(src, dst, log):
    if src.is_dir():
        shutil.copytree(src, dst, copy_function=shutil.copyfile, dirs_exist_ok=True)
        log.quiet('copied', src, '->', dst)
        return True
    return False


def report_body(text):
    """The JSON body of an .ips report (its first line is the header)."""
    return json.loads(text.split('\n', 1)[1])


# "userspace watchdog timeout: no successful checkins from com.apple.backboardd (2 induced crashes) ...", the kernel's
# "watchdog timeout: no checkins from watchdogd ..." and launchd's critical exits ("backboardd[53] exited -- ...").
PANIC_NAMES = (
    r'no successful checkins from (?:com\.apple\.)?([\w.-]+)',
    r'no checkins from ([\w.-]+)',
    r'^panic: +([\w.-]+)\[\d+\] exited',
)


def panic_process(panic_text):
    """The process a panic names (lower case), or None."""
    for pattern in PANIC_NAMES:
        match = re.search(pattern, panic_text, re.M)
        if match:
            return match.group(1).lower()
    return None


def thread_summary(report, process):
    """The threads of every task whose name is `process` (case-insensitive) in a stackshot report: state, wait, frames
    as image+offset (S: the dyld shared cache, slid base in binaryImages; K/U: the kernel)."""
    images = report.get('binaryImages', [])

    def frame(entry):
        index, offset = entry
        uuid, base, kind = images[index] if 0 <= index < len(images) else ('?', 0, '?')
        return '{}:{}+0x{:x}'.format(kind, uuid[:8], offset)

    lines = []
    for pid, task in sorted(report.get('processByPid', {}).items(), key=lambda item: int(item[0])):
        if (task.get('procname') or '').lower() != process:
            continue
        threads = task.get('threadById', {})
        lines.append('{}[{}]: {} threads'.format(task.get('procname'), pid, len(threads)))
        for wait in task.get('waitInfo', []):
            lines.append('  wait: ' + wait)
        for tid, thread in sorted(threads.items(), key=lambda item: int(item[0])):
            name = thread.get('name') or thread.get('dispatch_queue_label') or ''
            lines.append('  thread {} {} {}'.format(tid, name, ','.join(thread.get('state', []))))
            for kind, key in (('kernel', 'kernelFrames'), ('user', 'userFrames')):
                frames = thread.get(key) or []
                if frames:
                    lines.append('    {}: {}'.format(kind, ' '.join(frame(f) for f in frames)))
    return lines


def decode_stackshot(shot, kcdata, log, what):
    """<shot>.kcdata -> <stem>.stackshot.ips (kcdata.py -s); the .ips path, or None (logged)."""
    stem = shot.with_suffix('')
    if not kcdata or not Path(kcdata).is_file():
        log(what, 'stackshot', shot.name, 'NOT decoded: no kcdata.py (run as bazel run //deploy:pull_logs)')
        return None
    ips = stem.with_name(stem.name + '.stackshot.ips')
    result = subprocess.run(
        [sys.executable, '-I', str(kcdata), '-s', str(ips), str(shot)], capture_output=True, text=True
    )
    if result.returncode or not ips.is_file():
        log(what, 'stackshot', shot.name, 'NOT decoded:', (result.stderr or result.stdout).strip()[-300:])
        return None
    log(what, 'stackshot ->', ips)
    return ips


def decode_wakes(out, kcdata, log):
    """wake-*.kcdata (the wake guarantee's stackshots) -> .stackshot.ips and .threads.txt for SpringBoard and
    backboardd, the two that should have turned the panel on."""
    for shot in sorted(Path(out).glob('wake-*.kcdata')):
        ips = decode_stackshot(shot, kcdata, log, 'wake')
        if not ips:
            continue
        body = report_body(ips.read_text())
        lines = []
        for process in ('springboard', 'backboardd'):
            lines += thread_summary(body, process)
        if lines:
            stem = shot.with_suffix('')
            summary = stem.with_name(stem.name + '.threads.txt')
            summary.write_text(
                "# SpringBoard's and backboardd's threads in the wake stackshot {} (S: dyld shared cache "
                'offsets)\n{}\n'.format(ips.name, '\n'.join(lines))
            )
            log('wake stackshot threads ->', summary)


def decode_panics(out, kcdata, log):
    """panic-*.kcdata -> .stackshot.ips (kcdata.py -s) and .threads.txt for the process its panic string names."""
    for shot in sorted(Path(out).glob('panic-*.kcdata')):
        stem = shot.with_suffix('')
        ips = decode_stackshot(shot, kcdata, log, 'panic')
        if not ips:
            if not kcdata or not Path(kcdata).is_file():
                return
            continue
        text = stem.with_suffix('.log')
        process = panic_process(text.read_text(errors='replace')) if text.is_file() else None
        if not process:
            continue
        lines = thread_summary(report_body(ips.read_text()), process)
        if lines:
            summary = stem.with_name(stem.name + '.threads.txt')
            summary.write_text(
                "# {}'s threads in the panic stackshot {} (S: dyld shared cache offsets)\n{}\n".format(
                    process, ips.name, '\n'.join(lines)
                )
            )
            log('hung process', process, '->', summary)


def pull(data, out, log, logarchive=True, harvest=True, csstore=True, launchd=True, corpus=None, kcdata=None):
    data, out = Path(data), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    copytree(data / 'db/diagnostics', out / 'diagnostics', log)
    copytree(data / 'db/uuidtext', out / 'uuidtext', log)
    if launchd and (data / 'log/com.apple.xpc.launchd/launchd.log').is_file():
        shutil.copyfile(data / 'log/com.apple.xpc.launchd/launchd.log', out / 'launchd.log')
    crash = data / 'mobile/Library/Logs/CrashReporter'
    copytree(crash, out / 'mobile-CrashReporter', log)
    for report in sorted(crash.glob('com.apple.WebKit.*.ips')) if crash.is_dir() else []:
        shutil.copyfile(report, out / report.name)
    copytree(data / 'mobile/Library/Logs/mobileactivationd', out / 'mobileactivationd', log)
    own = renamed.data_folder(data)
    census = own / 'boot-io-census.bin'
    if census.is_file():
        shutil.copyfile(census, out / census.name)
    panics = sorted(own.glob('panic-*')) if own.is_dir() else []
    for panic in panics:
        shutil.copyfile(panic, out / panic.name)
    if panics:
        log('saved panics:', ', '.join(panic.name for panic in panics))
        decode_panics(out, kcdata, log)
    power = sorted(own.glob('power*.log')) if own.is_dir() else []
    wakes = sorted(own.glob('wake-*.kcdata')) if own.is_dir() else []
    for path in power + wakes:
        shutil.copyfile(path, out / path.name)
    if power:
        log('power journal:', ', '.join(path.name for path in power))
    if wakes:
        log('wake guarantee stackshots:', ', '.join(path.name for path in wakes))
        decode_wakes(out, kcdata, log)
    lines = []
    for name in ('staged_system_apps', 'datamigrator', 'Switch'):
        path = data / name
        lines.append('{}: {}'.format(name, sorted(p.name for p in path.iterdir()) if path.is_dir() else 'missing'))
    apps = data / 'containers/Bundle/Application'
    lines.append('installed apps: {}'.format(sum(1 for _ in apps.iterdir()) if apps.is_dir() else 0))
    (out / 'migration-state.txt').write_text('\n'.join(lines) + '\n')
    if csstore:
        stores = sorted(
            (data / 'mobile/Containers/Data/InternalDaemon').glob('*/Library/Caches/com.apple.LaunchServices-*.csstore')
        )
        if stores:
            counts = []
            for store in stores:
                shutil.copyfile(store, out / store.name)
            for ident in WEBKIT_IDS:
                total = sum(store.read_bytes().count(ident.encode()) for store in stores)
                counts.append('{} {}'.format(ident, total))
            (out / 'lsdb-webkit.txt').write_text('\n'.join(counts) + '\n')
        else:
            log('LaunchServices database NOT copied (not found)')
    script = workspace() / HARVEST / 'harvest.py' if HARVEST else None
    if harvest and script and script.is_file():
        corpus = Path(corpus or script.parent / 'corpus.log')
        result = subprocess.run(
            [sys.executable, str(script), str(corpus), '--volume', str(data), '--save', str(out / 'm2v')],
            capture_output=True,
            text=True,
        )
        log.quiet(result.stdout + result.stderr)
        log('m2v journals {} into {}'.format('harvested' if result.returncode == 0 else 'NOT harvested', corpus))
        if (out / BACKBOARDD_CACHE).is_dir():
            link = out / 'm2v-cache'
            if link.is_symlink() or link.exists():
                link.unlink()
            link.symlink_to(BACKBOARDD_CACHE)
    elif harvest:
        log('m2v harvest skipped: no', script)
    if logarchive and (out / 'diagnostics/timesync').is_dir():
        try:
            boots = mkarchive.build(out, out / 'boot.logarchive')
            log('logarchive: {} ({} boots)'.format(out / 'boot.logarchive', len(boots)))
        except SystemExit as error:
            log('logarchive NOT built:', error)
    log('logs ->', out)
    return out


def runfile(path):
    """A runfiles path (bazel run) or a plain one, absolute; None stays None."""
    if path and not os.path.isabs(path):
        try:
            from python.runfiles import runfiles

            path = runfiles.Create().Rlocation(path) or path
        except ImportError:
            pass
    return os.path.abspath(path) if path else None


def load_spec(path):
    """//install:spec from a runfiles path (bazel run) or a plain path."""
    import json

    if path and not os.path.isabs(path):
        try:
            from python.runfiles import runfiles

            path = runfiles.Create().Rlocation(path) or path
        except ImportError:
            pass
    if not path:
        return {'partition_bytes': 32 << 30, 'volumes': {'data': {'uuid': DATA_UUID}}}
    return json.loads(Path(path).read_text())


def data_volume(card, spec_data_uuid=DATA_UUID):
    for container in host.apfs_containers():
        for volume in container.get('Volumes', []):
            if volume.get('APFSVolumeUUID', '').upper() == spec_data_uuid:
                store = (container.get('PhysicalStores') or [{}])[0].get('DeviceIdentifier', '')
                if store.startswith(card.disk + 's'):
                    return volume['DeviceIdentifier']
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk')
    parser.add_argument('--out')
    parser.add_argument('--corpus', help="precompile corpus to merge into (default: the checkout's corpus.log)")
    parser.add_argument('--harvest', help="harvest.py's path in the checkout (//deploy passes it)")
    for flag in ('logarchive', 'harvest', 'csstore', 'launchd'):
        parser.add_argument('--no-' + flag, action='store_true')
    parser.add_argument('--log')
    parser.add_argument('--spec', help="//install:spec (the kernel's volume UUIDs)")
    parser.add_argument('--kcdata', help="XNU's tools/lldbmacros/kcdata.py (decodes a saved panic's stackshot)")
    parser.add_argument('--no-eject', action='store_true', help='leave the card attached (default: unmount and eject)')
    args = parser.parse_args(argv)
    global HARVEST
    HARVEST = args.harvest and str(Path(args.harvest).parent)
    log = host.Log(
        args.log
        or Path.home() / 'Library/Logs/switch-ios-install/pull-logs-{}.log'.format(time.strftime('%Y%m%d-%H%M%S'))
    )
    try:
        host.check_tools(log)
        spec = load_spec(args.spec)
        card = cards.resolve(args.disk)
        fat, target = cards.check_layout_unprivileged(card, spec)
        volume = data_volume(card, spec['volumes']['data']['uuid'])
        if not volume:
            raise host.HostError('no iPad Data volume {} on {}'.format(spec['volumes']['data']['uuid'], card.disk))
        out = Path(args.out or state_dir() / 'device-logs' / time.strftime('boot-%m%d-%H%M'))
        with cards.mounted(volume, log) as data:
            pull(
                data,
                out,
                log,
                logarchive=not args.no_logarchive,
                harvest=not args.no_harvest,
                csstore=not args.no_csstore,
                launchd=not args.no_launchd,
                corpus=args.corpus,
                kcdata=runfile(args.kcdata),
            )
        if not args.no_eject:
            # As deploy does: the iPad volumes unmounted, then the whole card ejected.
            container = host.container_on(target.identifier)
            if container:
                host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=log, check=False)
            host.run('diskutil', 'eject', card.disk, log=log)
            log('ejected', card.disk)
        return 0
    except cards.Refused as error:
        log('REFUSED:', error)
        return 2
    except host.HostError as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    sys.exit(main())
