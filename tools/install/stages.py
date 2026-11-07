"""//:image's work in stages, each kept with the key of what it was made from (keys.py).

Compose and pack run as the user, populate as root (file owners). composed.img stays in the scratch directory for the
next run; populate works on an APFS clone of it, which pack empties into NAME.SII.* as it goes.
"""

import argparse
import json
import os
import shutil
from pathlib import Path

from tools.install import (
    card as cards,
    compose,
    delta,
    folder,
    host,
    keys,
    manifest,
    pack,
    populate,
    pretranslate,
    progress,
)

STATE = 'state.json'
COMPOSED = 'composed.img'
POPULATED = 'populated.img'
PROOF = 'proof.img'
STAGING = '/private/preboot/Switch-Staging/'
FLOOR = 14 << 30
NOMINAL = 1 << 40
BLOCK = 4096
# A fitted container needs room for iOS to run in beyond what the image holds, and iOS a partition of this at least.
FREE_AFTER_INSTALL = 4 << 30
SMALLEST_PARTITION = 16 << 30
# iOS 17's image was composed before per-kind scratch, and the morning command names no --scratch, so 21H461 keeps the
# unsuffixed directory. Every other build gets its own, so one kind's image never recomposes over another's.
LEGACY_BUILD = '21H461'
# What the boot package pins of iOS 27's translation, kept from the deltas for //:image's folder (out/pretranslated).
KEPT = 'pretranslated'


def default_scratch(build: str) -> Path:
    base = Path.home() / 'Library/Caches/switch-ios'
    return base / 'image' if build == LEGACY_BUILD else base / f'image-{build}'


def version_label(pins: dict) -> str:
    return 'iPadOS {} ({})'.format(pins['version'], pins['build'])


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


def boot_files(package: str | Path) -> list[tuple[str, Path]]:
    """[(name, path)] of the version's boot entry, as install-ios.bin puts them in switchroot/ios/."""
    entry = cards.BOOT_ENTRY + '/'
    found = cards.boot_entry_files(package)
    return [(relative[len(entry) :], found[relative]) for relative in sorted(found) if relative.startswith(entry)]


def key_inputs(args: argparse.Namespace, spec: dict, pins: dict) -> dict:
    """Return what each stage is made from, as plain values for keys.compute."""
    members = pins['members']
    deltas = {r: getattr(args, 'delta_' + r, None) for r in pretranslate.ORDER}
    translated = [getattr(args, f'translated_{m}', None) for m in ('system_cryptex', 'app_cryptex')]
    return {
        'compose': {'os': members['os']['content_sha256'], 'volumes': spec['volumes'], 'nominal': NOMINAL},
        'populate': {
            'cryptexes': [members[m]['content_sha256'] for m in ('system_cryptex', 'app_cryptex')],
            'spec': spec['cryptexes'],
            'overlay': keys.tree_digest(Path(args.boot_package) / 'overlay'),
            'gpu': keys.tree_digest(args.gpu_base),
            'seed': keys.tree_digest(getattr(args, 'shader_seed', None)),
            'pretranslated_pins': keys.tree_digest(getattr(args, 'pretranslated_pins', None)),
            # A delta is its manifest (each replacement's sha256), a translated cryptex its sha256 (both Bazel's).
            'deltas': {r: keys.file_digest(Path(d) / f'{r}.manifest.json') for r, d in deltas.items() if d},
            'translated': [Path(t + '.sha256').read_text().strip() if t else None for t in translated],
        },
    }


def minimum_bytes(layout: dict) -> int:
    """Return the smallest partition to install to: what the container uses, room to run, and iOS's floor."""
    needed = -(-(layout['minimum_bytes'] + FREE_AFTER_INSTALL) // BLOCK) * BLOCK
    return min(layout['container_bytes'], max(SMALLEST_PARTITION, needed))


class Stages:
    def __init__(self, args: argparse.Namespace, spec: dict, pins: dict, log: host.Log):
        self.args, self.spec, self.pins, self.log = args, spec, pins, log
        self.scratch = Path(args.scratch)
        self.composed, self.populated = self.scratch / COMPOSED, self.scratch / POPULATED

    def state(self) -> dict:
        """{stage: key} of the stages whose output is still here, and the container's offset in the images."""
        try:
            saved = json.loads((self.scratch / STATE).read_text())
        except (OSError, ValueError):
            return {}
        found = {'offset': saved.get('offset')}
        for stage, image in (('compose', self.composed), ('populate', self.populated)):
            if image.is_file() and saved.get(stage):
                found[stage] = saved[stage]
        return found

    def save(self, **changes: object) -> None:
        saved = {k: v for k, v in self.state().items() if v is not None}
        saved.update(changes)
        (self.scratch / STATE).write_text(json.dumps(saved, indent=2) + '\n')
        give_back(self.scratch)

    def compose(self, key: str) -> None:
        self.scratch.mkdir(parents=True, exist_ok=True)
        for image in (self.composed, self.populated):
            image.unlink(missing_ok=True)
        offset, _ = compose.compose(self.composed, self.args.os_dmg, NOMINAL, self.spec, self.log)
        self.save(compose=key, populate=None, offset=offset)

    def deltas(self) -> pretranslate.Deltas | None:
        """IOS 27's deltas by role (Bazel's, install/translated.bzl), or None for an IPSW that needs none."""
        dirs = {r: getattr(self.args, 'delta_' + r, None) for r in pretranslate.ORDER}
        dirs = {r: d for r, d in dirs.items() if d}
        return pretranslate.Deltas(dirs, self.scratch / KEPT, self.log) if dirs else None

    def cryptexes(self) -> tuple[dict[str, str | Path], dict[str, str]]:
        """Return the cryptex images to stage, and the sha256 of those Bazel translated (pins describe the IPSW's)."""
        sources = {'system_cryptex': self.args.system_cryptex, 'app_cryptex': self.args.app_cryptex}
        expected = {}
        for member in sources:
            translated = getattr(self.args, 'translated_' + member, None)
            if translated:
                sources[member] = translated
                expected[member] = Path(translated + '.sha256').read_text().strip()
        return sources, expected

    def check_pins(self, deltas: pretranslate.Deltas) -> None:
        pins = getattr(self.args, 'pretranslated_pins', None)
        if pins:
            deltas.check_pins(json.loads(Path(pins).read_text()))
        else:
            self.log('pretranslate: no pins given; the trust modules are not checked against the boot package')

    def populate(self, key: str) -> None:
        self.populated.unlink(missing_ok=True)
        host.run('cp', '-c', str(self.composed), str(self.populated), log=self.log)
        deltas = self.deltas()
        if deltas:
            deltas.keep()
            self.check_pins(deltas)
        disk = compose.attach_raw(self.populated, self.log)
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
                sources, expected = self.cryptexes()
                populate.seed_data(system, data, True, self.log, reseed=True)
                if deltas and 'os' in deltas:
                    system_entries = deltas.entries('os')
                    below_var = delta.entries_under(system_entries, '/private/var')
                    delta.apply(data, below_var, self.log, 'Data')
                    rest = [e for e in system_entries if not e['path'].startswith('/private/var/')]
                    deltas.overlay_system(data, system, rest, True)
                images = populate.cryptex_images(self.spec, self.pins, sources, expected)
                populate.stage_cryptexes(preboot, self.spec, images, True, self.log)
                cards.sync_overlay(data, self.args.boot_package, self.log)
                populate.stage_gpu(data, self.args.gpu_stager, self.args.gpu_base, self.log)
                if self.args.shader_seed:
                    populate.merge_tree(self.args.shader_seed, data, True, self.log, 'shader seed')
            host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=self.log, check=False)
            compose.fsck(disk + 's1', 'image', self.log)
        finally:
            host.run('hdiutil', 'detach', disk, log=self.log, check=False)
        self.save(populate=key)

    def measure(self, tool: str, image: Path) -> dict:
        """apfs_tool's measure of the container in `image`, in bytes."""
        offset = self.state()['offset']
        blocks, minimum, pool, pool_blocks = map(int, host.run(tool, str(image), str(offset)).split())
        b = BLOCK
        return {
            'container_bytes': blocks * b,
            'minimum_bytes': minimum * b,
            'pool_start': pool * b,
            'pool_end': (pool + pool_blocks) * b,
        }

    def prove(self, tool: str, smallest: int) -> None:
        """Make install-ios.bin's fit on a clone of the real container; the kernel must attach it and fsck pass it."""
        proof = self.scratch / PROOF
        proof.unlink(missing_ok=True)
        with progress.step(f'pack: proving the fit to {progress.size(smallest)}', unit=None):
            self._prove(tool, proof, smallest)

    def _prove(self, tool: str, proof: Path, smallest: int) -> None:
        host.run('cp', '-c', str(self.populated), str(proof), log=self.log)
        try:
            host.run(tool, str(proof), str(self.state()['offset']), str(smallest // BLOCK), log=self.log)
            disk = compose.attach_raw(proof, self.log)
            try:
                if not host.container_on(disk + 's1'):
                    raise host.HostError(f'the container fitted to {smallest} bytes does not attach')
                host.run('diskutil', 'unmountDisk', disk + 's1', log=self.log, check=False)
                compose.fsck(disk + 's1', f'fitted to {smallest >> 20} MiB', self.log)
            finally:
                host.run('hdiutil', 'detach', disk, log=self.log, check=False)
        finally:
            proof.unlink(missing_ok=True)

    def pack(self, tool: str, out: Path, name: str, files: list[tuple[str, Path]]) -> manifest.Manifest:
        layout = self.measure(tool, self.populated)
        layout['minimum_bytes'] = minimum_bytes(layout)
        self.prove(tool, layout['minimum_bytes'])
        source = pack.Source(self.populated, self.state()['offset'], layout)
        target = pack.Target(Path(out) / folder.SII_DIR, folder.SII_DIR, name, version_label(self.pins), files)
        m = pack.pack(source, target, self.log)
        self.populated.unlink(missing_ok=True)
        self.save(populate=None)
        return m


def refresh_head(out: Path, name: str, files: list[tuple[str, Path]], label: str) -> manifest.Manifest:
    """Put new boot files into an image the folder holds: only NAME.SII.001's head is written again."""
    first = Path(out) / folder.SII_DIR / f'{name}.SII.001'
    with open(first, 'rb') as f:
        m = manifest.decode(f.read(manifest.BOOT_OFFSET))
    m.boot = pack.boot_records(files)
    m.label = label
    pack.write_head(Path(out) / folder.SII_DIR, m, files)
    return m


def boot_record(args: argparse.Namespace) -> dict[str, str | None]:
    """Return what the image's Data volume holds of the boot package: a later boot refresh must match it."""
    package = Path(args.boot_package)
    trust = Path(args.gpu_base) / 'TegraGM20BMetal.trustcache'
    return {
        'overlay': cards.tree_digest(package / 'overlay'),
        'gpu_trustcache': cards.sha256_file(trust) if trust.is_file() else None,
    }


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
