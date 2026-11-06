"""//:image's work in three stages, each resumable: compose (no root), populate (root, for file owners), pack.

The scratch directory's state.json records the last stage finished and what it was made from; a run whose inputs or
partition size differ starts again from compose.
"""

import argparse
import contextlib
import json
import os
import shutil
from collections.abc import Iterator
from pathlib import Path

from tools.install import (
    card as cards,
    compose,
    cryptex,
    delta,
    folder,
    host,
    manifest,
    pack,
    populate,
    pretranslate,
)

STATE = 'state.json'
IMAGE = 'image.img'
STAGES = ('compose', 'populate', 'pack')
STAGING = '/private/preboot/Switch-Staging/'
FLOOR = 14 << 30
# iOS 17's image was composed before per-kind scratch, and the morning command names no --scratch, so 21H461 keeps the
# unsuffixed directory. Every other build gets its own, so one kind's image never recomposes over another's.
LEGACY_BUILD = '21H461'
# Kept translations (pretranslate's memo): --warm-translations fills it as the user, --translation-memo reads it.
TRANSLATIONS = 'translations'


def default_scratch(build: str) -> Path:
    base = Path.home() / 'Library/Caches/switch-ios'
    return base / 'image' if build == LEGACY_BUILD else base / f'image-{build}'


def identity(_args: argparse.Namespace, spec: dict, pins: dict, size: int) -> dict:
    """Return what a composed image depends on besides the boot package (populate reads that afresh)."""
    return {
        'partition_bytes': size,
        'build': pins['build'],
        'os': pins['members']['os']['content_sha256'],
        'volumes': {k: v['uuid'] for k, v in spec['volumes'].items()},
    }


def ipsw_volumes(spec: dict, pins: dict) -> dict:
    """Return the spec with the selected IPSW's own System and cryptex volumes, where its pins name them.

    The pins' "volumes" (ipsw/pins.bzl); the kernel takes the same values from them.
    """
    volumes = pins.get('volumes')
    if not volumes:
        return spec
    spec = json.loads(json.dumps(spec))
    spec['volumes']['system']['uuid'] = volumes['os_volume_uuid'].upper()
    for item in spec['cryptexes']:
        item['volume_uuid'] = volumes[item['member'] + '_uuid'].upper()
        item['path'] = STAGING + volumes[item['member'] + '_image']
    return spec


class Stages:
    def __init__(self, args: argparse.Namespace, spec: dict, pins: dict, log: host.Log):
        self.args, self.spec, self.pins, self.log = args, spec, pins, log
        self.scratch = Path(args.scratch)
        self.image = self.scratch / IMAGE

    def state(self, size: int, refuse_other_size: bool = False) -> tuple[str | None, int | None]:
        path = self.scratch / STATE
        if not path.is_file() or not self.image.is_file():
            return None, None
        saved = json.loads(path.read_text())
        want = identity(self.args, self.spec, self.pins, size)
        if saved.get('identity') != want:
            other = saved.get('identity', {}).get('partition_bytes')
            if refuse_other_size and other != size and dict(saved['identity'], partition_bytes=size) == want:
                raise host.HostError(
                    f'{self.image} was composed for a {other >> 20} MiB partition; '
                    f"this card's is {size >> 20} MiB. Give that card, or delete {self.scratch / STATE} to start again"
                )
            self.log(f'scratch: {self.image} was made from other inputs; starting again')
            return None, None
        return saved['stage'], saved['offset']

    def save(self, stage: str, size: int, offset: int) -> None:
        record = {'stage': stage, 'offset': offset, 'identity': identity(self.args, self.spec, self.pins, size)}
        (self.scratch / STATE).write_text(json.dumps(record, indent=2) + '\n')
        give_back(self.scratch)

    def clear(self) -> None:
        for name in (IMAGE, STATE):
            if (self.scratch / name).exists():
                (self.scratch / name).unlink()

    def compose(self, size: int) -> int:
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.clear()
        offset, _ = compose.compose(self.image, self.args.os_dmg, size, self.spec, self.log)
        self.save('compose', size, offset)
        return offset

    def roles(self) -> pretranslate.Roles | None:
        """IOS 27's pretranslate drivers by role (pretranslate.py), or None for an IPSW that needs none."""
        given = getattr(self.args, 'pretranslate', None)
        memo = self.scratch / TRANSLATIONS if getattr(self.args, 'translation_memo', False) else None
        return pretranslate.Roles(given, self.scratch, self.log, memo) if given else None

    def warm_translations(self) -> None:
        """Translate every role from the decrypted members, mounted read-only as the calling user, into the memo.

        The members hold the same files populate later mounts, so its run with --translation-memo only copies them.
        """
        given = getattr(self.args, 'pretranslate', None)
        if not given:
            raise host.HostError('this IPSW kind has no pretranslate roles to warm')
        roles = pretranslate.Roles(given, self.scratch, self.log, self.scratch / TRANSLATIONS)
        members = {
            'os': self.args.os_dmg,
            'app_cryptex': self.args.app_cryptex,
            'system_cryptex': self.args.system_cryptex,
        }
        for role in pretranslate.ORDER:
            if role in roles:
                with self._member_volume(role, members[role]) as root:
                    roles.run(role, root)
                roles.done(role)

    @contextlib.contextmanager
    def _member_volume(self, role: str, image: str) -> Iterator[Path]:
        if role == 'os':
            whole, store = compose._attach_os(image, self.log)
            try:
                container = host.container_on(store[len('/dev/') :]) or {}
                uuid = self.spec['volumes']['system']['uuid'].upper()
                volume = next(
                    v['DeviceIdentifier'] for v in container.get('Volumes', []) if v['APFSVolumeUUID'].upper() == uuid
                )
                with cards.mounted(volume, self.log, read_only=True) as root:
                    yield root
            finally:
                host.run('hdiutil', 'detach', whole, log=self.log, check=False)
        else:
            volume, whole = cryptex._attach(Path(image), self.log, writable=False)
            try:
                with cards.mounted(volume, self.log, read_only=True) as root:
                    yield root
            finally:
                host.run('hdiutil', 'detach', whole, log=self.log, check=False)

    def cryptexes(self, roles: pretranslate.Roles | None) -> tuple[dict[str, str | Path], dict[str, str]]:
        """Return the cryptex images to stage, and their sha256 where translated (the pins describe the IPSW's).

        The App cryptex goes before the SystemOS one, whose dyld cache takes the other volumes' cdhashes.
        """
        sources = {'system_cryptex': self.args.system_cryptex, 'app_cryptex': self.args.app_cryptex}
        expected = {}
        for member in ('app_cryptex', 'system_cryptex'):
            if roles and member in roles:
                sources[member] = cryptex.rebuild(
                    sources[member], lambda root, m=member: roles.run(m, root), self.scratch, self.log, member
                )
                roles.done(member)
                expected[member] = cards.sha256_file(sources[member])
        return sources, expected

    def check_pins(self, roles: pretranslate.Roles) -> None:
        pins = getattr(self.args, 'pretranslated_pins', None)
        if pins:
            roles.check_pins(json.loads(Path(pins).read_text()))
        else:
            self.log('pretranslate: no pins given; the trust modules are not checked against the boot package')

    def check_translate_space(self, roles: pretranslate.Roles | None) -> None:
        """Check for the space translating iOS 27 needs in the scratch directory.

        The SystemOS cryptex's clone takes its translated files, and the rebuilt image is staged into Preboot.
        """
        if not roles or 'system_cryptex' not in roles:
            return
        want = 2 * os.path.getsize(self.args.system_cryptex) + (1 << 30)
        free = free_bytes(self.scratch)
        if free < want + FLOOR:
            raise host.HostError(
                f'translating iOS 27 needs about {want >> 30} GiB free in {self.scratch} besides the '
                f'{FLOOR >> 30} GiB kept free; {free >> 30} GiB is'
            )

    def populate(self, size: int, offset: int) -> None:
        roles = self.roles()
        self.check_translate_space(roles)
        disk = compose.attach_raw(self.image, self.log)
        try:
            container = host.container_on(disk + 's1')
            vols = {v['APFSVolumeUUID'].upper(): v['DeviceIdentifier'] for v in (container or {}).get('Volumes', [])}
            dev = {k: vols.get(v['uuid']) for k, v in self.spec['volumes'].items()}
            if not all(dev.values()):
                raise host.HostError(f"the composed container's volumes did not attach: {dev}")
            with (
                cards.mounted(dev['system'], self.log, read_only=True) as system,
                cards.mounted(dev['data'], self.log, owners=True) as data,
                cards.mounted(dev['preboot'], self.log, owners=True) as preboot,
            ):
                system_entries = roles.run('os', system) if roles and 'os' in roles else None
                sources, expected = self.cryptexes(roles)
                if roles:
                    self.check_pins(roles)
                populate.seed_data(system, data, True, self.log, reseed=True)
                if system_entries is not None:
                    below_var = delta.entries_under(system_entries, '/private/var')
                    delta.apply(data, below_var, self.log, 'Data')
                    rest = [e for e in system_entries if not e['path'].startswith('/private/var/')]
                    roles.overlay_system(data, system, rest, True)
                    roles.done('os')
                populate.stage_cryptexes(preboot, self.spec, self.pins, sources, True, self.log, expected)
                cards.sync_overlay(data, self.args.boot_package, self.log)
                populate.stage_gpu(data, self.args.gpu_stager, self.args.gpu_base, self.log)
                if self.args.shader_seed:
                    populate.merge_tree(self.args.shader_seed, data, True, self.log, 'shader seed')
            host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=self.log, check=False)
            compose.fsck(disk + 's1', 'image', self.log)
        finally:
            host.run('hdiutil', 'detach', disk, log=self.log, check=False)
        self.save('populate', size, offset)

    def pack(self, size: int, offset: int, install_dir: Path, boot: list[manifest.Boot]) -> manifest.Manifest:
        prefix = folder.kind_dir(self.pins['build'])
        m = pack.pack(self.image, offset, size, install_dir, prefix, version_label(self.pins), boot, self.log)
        self.clear()
        return m


def boot_record(args: argparse.Namespace) -> dict[str, str | None]:
    """Return what the image's Data volume holds of the boot package: a later boot refresh must match it."""
    package = Path(args.boot_package)
    trust = Path(args.gpu_base) / 'TegraGM20BMetal.trustcache'
    return {
        'overlay': cards.tree_digest(package / 'overlay'),
        'gpu_trustcache': cards.sha256_file(trust) if trust.is_file() else None,
    }


def version_label(pins: dict) -> str:
    return 'iPadOS {} ({})'.format(pins['version'], pins['build'])


def boot_records(args: argparse.Namespace, build: str) -> list[manifest.Boot]:
    """Return the manifest's records of the kind's boot files, as add_boot() puts them in the kind's folder."""
    files = folder.boot_files(args.boot_package, build)
    return [pack.boot_file(relative, source) for relative, source in files.items() if folder.kind_of(relative)]


def add_boot(out: Path, args: argparse.Namespace, build: str) -> None:
    for relative, source in folder.boot_files(args.boot_package, build).items():
        folder.add(out, relative, source)
    folder.add(out, folder.PAYLOAD, args.install_payload)


def refresh_boot(out: str | Path, args: argparse.Namespace, build: str, log: host.Log) -> None:
    """Only the kind's boot entry and install-ios.bin, into a folder //:image made, when Data's parts still match.

    The image's manifest records the boot files, so it is written again with theirs.
    """
    out = Path(out)
    record = folder.records(out).get(build)
    path = out / folder.kind_dir(build) / folder.MANIFEST
    if not record or not path.is_file():
        raise host.HostError(f'{out} holds no {build} image //:image finished: run //:image first')
    current = boot_record(args)
    changed = [k for k in current if current[k] != record['boot'].get(k)]
    if changed:
        raise host.HostError(
            "the boot package's {} changed since the image was made (it lives on Data): run //:image again".format(
                ', '.join(changed)
            )
        )
    m = manifest.decode(path.read_bytes())
    image_files = {folder.MANIFEST} | {Path(p.path).name for p in m.parts}
    for stale in sorted((out / folder.kind_dir(build)).iterdir()):
        if stale.is_file() and stale.name not in image_files:
            stale.unlink()
    for relative in folder.SHARED:
        (out / relative).unlink(missing_ok=True)
    add_boot(out, args, build)
    m.boot = boot_records(args, build)
    path.write_bytes(manifest.encode(m))
    give_back(out)
    log(f'folder: the {build} boot entry, its manifest and install-ios.bin refreshed in {out}')


def give_back(path: Path) -> None:
    """Files made under sudo go back to the user who ran //:image."""
    uid, gid = os.environ.get('SUDO_UID'), os.environ.get('SUDO_GID')
    if os.geteuid() != 0 or not uid:
        return
    for p in [Path(path), *Path(path).rglob('*')]:
        os.chown(p, int(uid), int(gid))


def free_bytes(path: str | Path) -> int:
    path = Path(path)
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free
