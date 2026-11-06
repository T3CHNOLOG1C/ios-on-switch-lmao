"""//:image's work in three stages, each resumable: compose (no root), populate (root, for file owners), pack.

The scratch directory's state.json records the last stage finished and what it was made from; a run whose inputs or
partition size differ starts again from compose.
"""

import json
import os
import shutil
from pathlib import Path

from tools.install import card as cards, compose, cryptex, delta, folder, host, pack, populate, pretranslate

STATE = 'state.json'
IMAGE = 'image.img'
STAGES = ('compose', 'populate', 'pack')
STAGING = '/private/preboot/Switch-Staging/'
FLOOR = 14 << 30


def identity(args, spec, pins, size):
    """What a composed image depends on besides the boot package (populate reads that afresh)."""
    return {
        'partition_bytes': size,
        'build': pins['build'],
        'os': pins['members']['os']['content_sha256'],
        'volumes': {k: v['uuid'] for k, v in spec['volumes'].items()},
    }


def ipsw_volumes(spec, pins):
    """The spec with the selected IPSW's own System and cryptex volumes, where its pins name them (ipsw/pins.bzl
    "volumes"; the kernel takes the same values from them).
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
    def __init__(self, args, spec, pins, log):
        self.args, self.spec, self.pins, self.log = args, spec, pins, log
        self.scratch = Path(args.scratch)
        self.image = self.scratch / IMAGE

    def state(self, size, refuse_other_size=False):
        path = self.scratch / STATE
        if not path.is_file() or not self.image.is_file():
            return None, None
        saved = json.loads(path.read_text())
        want = identity(self.args, self.spec, self.pins, size)
        if saved.get('identity') != want:
            other = saved.get('identity', {}).get('partition_bytes')
            if refuse_other_size and other != size and dict(saved['identity'], partition_bytes=size) == want:
                raise host.HostError(
                    f"{self.image} was composed for a {other >> 20} MiB partition; this card's is {size >> 20} MiB. Give that "
                    f'card, or delete {self.scratch / STATE} to start again'
                )
            self.log(f'scratch: {self.image} was made from other inputs; starting again')
            return None, None
        return saved['stage'], saved['offset']

    def save(self, stage, size, offset):
        record = {'stage': stage, 'offset': offset, 'identity': identity(self.args, self.spec, self.pins, size)}
        (self.scratch / STATE).write_text(json.dumps(record, indent=2) + '\n')
        give_back(self.scratch)

    def clear(self):
        for name in (IMAGE, STATE):
            if (self.scratch / name).exists():
                (self.scratch / name).unlink()

    def compose(self, size):
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.clear()
        offset, _ = compose.compose(self.image, self.args.os_dmg, size, self.spec, self.log)
        self.save('compose', size, offset)
        return offset

    def roles(self):
        """IOS 27's pretranslate drivers by role (pretranslate.py), or None for an IPSW that needs none."""
        given = getattr(self.args, 'pretranslate', None)
        return pretranslate.Roles(given, self.scratch, self.log) if given else None

    def cryptexes(self, roles):
        """The cryptex images to stage, and their sha256 where they were translated (the pins describe the IPSW's).
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

    def check_pins(self, roles):
        pins = getattr(self.args, 'pretranslated_pins', None)
        if pins:
            roles.check_pins(json.loads(Path(pins).read_text()))
        else:
            self.log('pretranslate: no pins given; the trust modules are not checked against the boot package')

    def check_translate_space(self, roles):
        """The SystemOS cryptex's clone takes its translated files, and the rebuilt image is staged into Preboot."""
        if not roles or 'system_cryptex' not in roles:
            return
        want = 2 * os.path.getsize(self.args.system_cryptex) + (1 << 30)
        free = free_bytes(self.scratch)
        if free < want + FLOOR:
            raise host.HostError(
                f'translating iOS 27 needs about {want >> 30} GiB free in {self.scratch} besides the {FLOOR >> 30} GiB kept free; {free >> 30} GiB is'
            )

    def populate(self, size, offset):
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

    def pack(self, size, offset, install_dir, label):
        m = pack.pack(self.image, offset, size, install_dir, folder.INSTALL, label, self.log)
        self.clear()
        return m


def boot_record(args):
    """What the image's Data volume holds of the boot package: a later boot refresh must match it."""
    package = Path(args.boot_package)
    trust = Path(args.gpu_base) / 'TegraGM20BMetal.trustcache'
    return {
        'overlay': cards.tree_digest(package / 'overlay'),
        'gpu_trustcache': cards.sha256_file(trust) if trust.is_file() else None,
    }


def add_boot(out, args, payload):
    folder.add_boot_package(out, args.boot_package)
    folder.add(out, payload, args.install_payload)


def refresh_boot(out, args, payload, log):
    """Only the boot entry and install-ios.bin, into a folder //:image made, when Data's parts still match."""
    out = Path(out)
    try:
        record = json.loads((out / folder.MARKER).read_text())
    except (OSError, ValueError):
        raise host.HostError(f'{out} is not a folder //:image finished: run //:image first') from None
    current = boot_record(args)
    changed = [k for k in current if current[k] != record['boot'].get(k)]
    if changed:
        raise host.HostError(
            "the boot package's {} changed since the image was made (it lives on Data): run //:image again".format(
                ', '.join(changed)
            )
        )
    install = out / folder.INSTALL
    for path in sorted(out.rglob('*')):
        if path.is_file() and install not in path.parents and path.name != folder.MARKER:
            path.unlink()
    add_boot(out, args, payload)
    give_back(out)
    log(f'folder: boot entry and install-ios.bin refreshed in {out}')


def give_back(path):
    """Files made under sudo go back to the user who ran //:image."""
    uid, gid = os.environ.get('SUDO_UID'), os.environ.get('SUDO_GID')
    if os.geteuid() != 0 or not uid:
        return
    for p in [Path(path), *Path(path).rglob('*')]:
        os.chown(p, int(uid), int(gid))


def free_bytes(path):
    path = Path(path)
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free
