#!/usr/bin/env python3
"""Prepare iOS for a Switch: a folder to merge onto the SD card's root, holding the boot entry, install-ios.bin and the
install image it writes onto the card's Linux or Android partition.

    bazel run //:image [-- --disk diskN | --size 64G] [--partition NAME] [--out DIR] [--copy-to-card]
    bazel run //:image_dry_run -- [--disk diskN | --size 64G]

The image is made for one partition size: the card's, read from its partition table (diskutil, no root) when it is
in the Mac, else --size (G and M are GiB and MiB, as hekate's partition manager counts). install-ios.bin refuses any
other size. Composing and populating the container need root for file owners, so the run asks for sudo once.
"""

import argparse
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.install import (
    apfs,
    card as cards,
    compose,
    extents,
    folder,
    gpt,
    host,
    manifest,
    stages,
    target as targets,
)

INPUTS = (
    'spec',
    'ipsw',
    'os_dmg',
    'system_cryptex',
    'app_cryptex',
    'boot_package',
    'gpu_base',
    'gpu_stager',
    'shader_seed',
    'shader_seed_size',
    'install_payload',
)
DELTAS = ('os', 'system_cryptex', 'app_cryptex')
PAYLOAD = 'bootloader/payloads/install-ios.bin'
MIN_BYTES = 16 << 30
HEADROOM = 4 << 30
DATA_SEED_BYTES = 1 << 30
FLOOR = 14 << 30


def parse_size(text):
    found = re.fullmatch(r'\s*(\d+)\s*([GMgm]?)(?:i?B)?\s*', text)
    if not found:
        raise argparse.ArgumentTypeError('{!r}: give bytes, or a number with G (GiB) or M (MiB)'.format(text))
    return int(found.group(1)) << {'': 0, 'm': 20, 'g': 30}[found.group(2).lower()]


def resolve_inputs(args):
    try:
        from python.runfiles import runfiles

        locate = runfiles.Create()
    except ImportError:
        locate = None
    for listing in args.inputs or []:
        path = locate.Rlocation(listing) if locate and not os.path.isabs(listing) else listing
        for name, value in json.loads(Path(path).read_text()).items():
            if not getattr(args, name, None):
                setattr(args, name, value)
    for name in INPUTS:
        value = getattr(args, name, None)
        if value and not os.path.isabs(value) and locate:
            value = locate.Rlocation(value) or value
        setattr(args, name, os.path.abspath(value) if value else None)
    if args.gpu_base and os.path.isfile(args.gpu_base):
        args.gpu_base = os.path.dirname(args.gpu_base)
    args.deltas = {}
    for role in DELTAS:
        value = getattr(args, 'delta_' + role, None)
        if value:
            value = os.path.abspath(locate.Rlocation(value) if locate and not os.path.isabs(value) else value)
            setattr(args, 'delta_' + role, value)
            args.deltas[role] = value


def switch_cards():
    found = []
    for item in host.plist('diskutil', 'list', '-plist', 'physical').get('WholeDisks', []):
        try:
            card = cards.resolve(item)
        except cards.Refused:
            continue
        parts = cards.diskutil_partitions(card)
        if parts and parts[0].name == 'hos_data':
            found.append(card)
    return found


def find_target(args, spec, log):
    """(card or None, FAT partition or None, target partition or None, partition bytes)."""
    if args.size:
        targets.check_size(args.size)
        return None, None, None, args.size
    if args.disk:
        card = cards.resolve(args.disk)
    else:
        found = switch_cards()
        if len(found) != 1:
            raise cards.Refused(
                '{} Switch SD cards in this Mac ({}): insert one, name it with --disk, or give --size'.format(
                    len(found), ', '.join(c.disk for c in found) or 'none'
                )
            )
        card = found[0]
    fat, partition = cards.check_layout_unprivileged(card, spec, args.partition)
    log(
        'card: {} ({} bytes); target {} {!r}, {} bytes'.format(
            card.disk, card.size, partition.identifier, partition.name, partition.size
        )
    )
    return card, fat, partition, partition.size


def needed(args, pins, os_bytes):
    seed = 0
    if args.shader_seed_size:
        seed = json.loads(Path(args.shader_seed_size).read_text())['blocks4k']
    cryptexes = sum(pins['members'][m]['size'] for m in ('system_cryptex', 'app_cryptex'))
    return os_bytes + cryptexes + DATA_SEED_BYTES + seed + HEADROOM


def version_label(pins):
    return 'iPadOS {} ({})'.format(pins['version'], pins['build'])


def plan_lines(args, size, card, done):
    where = args.out
    parts = "{}'s FAT".format(card.disk) if args.copy_to_card else where
    steps = [
        ('compose', 'compose a {} MiB iOS container in {}'.format(size >> 20, args.scratch)),
        ('populate', 'populate Data and Preboot (asks for sudo once)'),
        ('pack', 'pack it into {}/{}/ and add the boot entry and install-ios.bin'.format(parts, folder.INSTALL)),
    ]
    if args.copy_to_card:
        steps.append(('pack', "merge {} onto {}'s FAT and eject it".format(where, card.disk)))
    skip = stages.STAGES[: stages.STAGES.index(done) + 1] if done else ()
    stop = stages.STAGES.index(args.stop_after) + 1 if args.stop_after else len(stages.STAGES)
    return [('done: ' if stage in skip else '') + line for stage, line in steps if stage in stages.STAGES[:stop]]


def check_inputs(args):
    missing = [
        name
        for name in ('spec', 'ipsw', 'boot_package', 'gpu_base', 'gpu_stager', 'install_payload')
        if not getattr(args, name) or not os.path.exists(getattr(args, name))
    ]
    if not args.dry_run and not args.boot_only:
        missing += [
            name
            for name in ('os_dmg', 'system_cryptex', 'app_cryptex')
            if not getattr(args, name) or not os.path.exists(getattr(args, name))
        ]
    if missing:
        raise host.HostError('inputs missing: {}'.format(', '.join(missing)))
    return sorted(cards.boot_entry_files(args.boot_package)) + [PAYLOAD]


class Builder:
    def __init__(self, args, log):
        self.args, self.log = args, log
        self.pins = json.loads(Path(args.ipsw).read_text())
        self.spec = stages.ipsw_volumes(json.loads(Path(args.spec).read_text()), self.pins)

    def run(self):
        sd_files = check_inputs(self.args)
        if self.args.boot_only:
            stages.refresh_boot(self.args.out, self.args, PAYLOAD, self.log)
            return 0
        self.log('image: {}'.format(version_label(self.pins)))
        card, fat, partition, size = find_target(self.args, self.spec, self.log)
        if size < MIN_BYTES:
            raise cards.Refused(
                '{} bytes is too small for iOS: make the partition at least {} GiB'.format(size, MIN_BYTES >> 30)
            )
        if self.args.copy_to_card and not card:
            raise cards.Refused('--copy-to-card writes to the card: put it in the Mac instead of giving --size')
        work = stages.Stages(self.args, self.spec, self.pins, self.log)
        done, offset = work.state(size, refuse_other_size=self.args.copy_to_card)
        for line in plan_lines(self.args, size, card, done):
            self.log('  plan:', line)
        for relative in sd_files:
            self.log('  card:', relative)
        if self.args.dry_run:
            self.log('dry run: nothing written')
            return 0
        if done is None:
            self.check_space(size)
            offset = work.compose(size)
            done = 'compose'
            self.log('compose: done; the image is {} MiB on disk'.format(allocated(work.image) >> 20))
        if self.args.stop_after == 'compose':
            return 0
        if done == 'compose':
            if os.geteuid() != 0:
                reexec_as_root(self.args)
            work.populate(size, offset)
            done = 'populate'
        if self.args.stop_after == 'populate':
            return 0
        out = folder.prepare(self.args.out)
        if self.args.copy_to_card:
            self.pack_to_card(work, card, fat, size, offset, out)
        else:
            m = work.pack(size, offset, out / folder.INSTALL, version_label(self.pins))
            self.finish(out, m)
            self.log("folder: {} ({} files); merge it onto the SD card's root".format(out, len(folder.files(out))))
        return 0

    def check_space(self, size):
        want = needed(self.args, self.pins, compose.os_container_bytes(self.args.os_dmg, self.log))
        if size < want:
            raise cards.Refused('the partition is {} bytes; this iOS needs at least {}'.format(size, want))
        free = stages.free_bytes(self.args.scratch)
        if free < want + FLOOR:
            raise host.HostError(
                '{} has {} GiB free; composing needs about {} GiB and leaves {} GiB'.format(
                    self.args.scratch, free >> 30, want >> 30, FLOOR >> 30
                )
            )

    def finish(self, out, m):
        record = {'label': m.label, 'partition_bytes': m.partition_bytes, 'boot': stages.boot_record(self.args)}
        (Path(out) / folder.MARKER).write_text(json.dumps(record, indent=2) + '\n')
        stages.add_boot(out, self.args, PAYLOAD)
        stages.give_back(out)

    def pack_to_card(self, work, card, fat, size, offset, out):
        """The parts go straight onto the card's FAT: no second local copy of the image."""
        with cards.mounted(fat.identifier, self.log) as root:
            install = Path(root) / folder.INSTALL
            if install.exists():
                shutil.rmtree(install)
            install.mkdir(parents=True)
            free = shutil.disk_usage(root).free
            if free < allocated(work.image):
                raise host.HostError(
                    "the card's FAT has {} MiB free; the image needs about {} MiB".format(
                        free >> 20, allocated(work.image) >> 20
                    )
                )
            m = work.pack(size, offset, install, version_label(self.pins))
            self.finish(out, m)
            folder.merge(out, root, self.log, keep=[p.path for p in m.parts] + [folder.MANIFEST_PATH])
        host.run('diskutil', 'eject', card.disk, log=self.log)
        self.log('ejected', card.disk, "- in the Switch, run install-ios.bin from hekate's Payloads")


def allocated(path):
    return os.stat(path).st_blocks * 512 if os.path.exists(path) else 0


def reexec_as_root(args):
    command = ['sudo', '--', sys.executable, os.path.abspath(__file__)]
    for key, value in sorted(vars(args).items()):
        if key in ('inputs', 'deltas'):
            continue
        flag = '--' + key.replace('_', '-')
        if value is True:
            command.append(flag)
        elif value not in (None, False):
            command.append('{}={}'.format(flag, value))
    print('composing needs root for file owners; running under sudo', flush=True)
    os.execvp('sudo', command)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = parser.add_mutually_exclusive_group()
    where.add_argument('--disk', help="the Switch SD card's whole disk (default: the one Switch card in this Mac)")
    where.add_argument('--size', type=parse_size, help="the partition's exact size, when the card is not in the Mac")
    parser.add_argument('--partition', help='the partition by GPT name, when the card has Linux and Android')
    parser.add_argument('--out', help='the folder to write (default: out/sd in the source tree)')
    parser.add_argument(
        '--copy-to-card',
        action='store_true',
        help='write the image and folder onto the card, then eject it (no local copy of the image)',
    )
    parser.add_argument('--boot-only', action='store_true', help="refresh only the boot entry in --out's folder")
    parser.add_argument('--stop-after', choices=stages.STAGES[:2], help='stop after this stage; rerun to go on')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--keep-scratch', action='store_true')
    parser.add_argument('--scratch', help='where the container is composed (needs its size in free space, sparse)')
    parser.add_argument('--log')
    parser.add_argument('--inputs', action='append', help='a JSON file of the inputs below (//install:image_inputs)')
    for name in INPUTS + tuple('delta_' + role for role in DELTAS):
        parser.add_argument('--' + name.replace('_', '-'))
    args = parser.parse_args(argv)
    resolve_inputs(args)
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY') or os.getcwd()
    args.out = os.path.abspath(args.out or os.path.join(workspace, 'out/sd'))
    args.scratch = os.path.abspath(args.scratch or Path.home() / 'Library/Caches/switch-ios/image')
    args.log = args.log or str(
        Path.home() / 'Library/Logs/switch-ios/image-{}.log'.format(time.strftime('%Y%m%d-%H%M%S'))
    )
    log = host.Log(args.log) if not args.dry_run else PrintLog()
    try:
        host.check_tools(log)
        return Builder(args, log).run()
    except cards.Refused as error:
        log('REFUSED:', error)
        return 2
    except (host.HostError, apfs.ApfsError, gpt.GptError, extents.ExtentError, manifest.ManifestError) as error:
        log('FAILED:', error)
        return 1
    finally:
        log.quiet('log:', args.log)


class PrintLog:
    def __call__(self, *parts):
        print(*parts, flush=True)

    def quiet(self, *parts):
        pass


if __name__ == '__main__':
    sys.exit(main())
