"""iOS 27's user space, translated to plain arm64 while the image is composed (//translate/arm64e/pretranslate).

Each role's driver reads one mounted volume and writes its delta (delta.py) to a scratch directory. The SystemOS
cryptex goes last: the prebuilt loaders in its dyld cache check the binaries of every volume by cdhash, so its driver
takes the cdhash pairs the System volume's and the App cryptex's runs wrote, or each launch falls back to a slower
loader.

The System volume stays sealed as Apple shipped it. Its translated files go to Data under Switch/pretranslated, at
their System paths, its own tree because a deploy replaces Switch/overlay whole; the kernel shows them at those paths
from the tree's own index (overlay_index.py), made from Switch/pretranslated.json and the files' ids once they are
placed.
"""

import json
import os
import shutil
import stat
import subprocess
from pathlib import Path

from tools.install import card as cards, delta, host, overlay_index, renamed

OVERLAY = renamed.DATA_FOLDER + '/pretranslated'
OVERLAY_MANIFEST = OVERLAY + '.json'
OVERLAY_INDEX = OVERLAY + '/' + overlay_index.NAME
COMPRESSED = 0x20

ORDER = ('os', 'app_cryptex', 'system_cryptex')
# What the boot package needs of a run (trust modules, the device tree's list of translated cache cdhashes).
KEPT = ('{role}.trustcache', '{role}.cdhash-pairs', 'a64x-cache-pretranslated.bin')


def _cdhash(cdhash: list[str]) -> bytes:
    return bytes.fromhex(cdhash[0]) if cdhash else b'\x00' * overlay_index.CDHASH


def _place(root: Path, e: dict, original: os.stat_result, owners: bool) -> Path:
    """Move e's translated file to its System path below root, with the System file's mode, owner and flags."""
    target = root / e['path'].lstrip('/')
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name('.partial-' + target.name)
    shutil.copyfile(e['source'], partial)
    if cards.sha256_file(partial) != e['sha256_out']:
        partial.unlink()
        raise host.HostError('{}: the translated file does not match its manifest'.format(e['path']))
    os.replace(partial, target)
    e['source'].unlink()
    target.chmod(original.st_mode & 0o7777)
    if owners:
        os.chown(target, original.st_uid, original.st_gid)
    if original.st_flags & ~COMPRESSED:
        os.chflags(target, original.st_flags & ~COMPRESSED)
    return target


class Roles:
    def __init__(self, roles, scratch, log, memo_root=None):
        """`roles`: {role: {"driver": path, "tool": path, "trustcaches": [path]}}.

        `memo_root`, if given, keeps each role's translations there across runs (the driver's --memo); done() leaves it.
        """
        unknown = set(roles) - set(ORDER)
        if unknown:
            raise host.HostError('unknown pretranslate roles: {}'.format(sorted(unknown)))
        self.roles, self.scratch, self.log = roles, Path(scratch), log
        self.kept = self.scratch / 'pretranslated'
        self.memo_root = Path(memo_root) if memo_root else None

    def __contains__(self, role):
        return role in self.roles

    def out(self, role):
        return self.scratch / ('delta-' + role)

    def run(self, role, volume):
        """The role's delta for the volume mounted at `volume`; its entries are ready for delta.apply."""
        if role == 'system_cryptex':
            missing = [r for r in ('os', 'app_cryptex') if r in self.roles and not self._pairs(r).is_file()]
            if missing:
                raise host.HostError('system_cryptex is translated after {}'.format(', '.join(missing)))
        spec = self.roles[role]
        command = [spec['driver'], '--role', role, '--tool', spec['tool'], '--volume', str(volume)]
        command += ['--out', str(self.out(role))]
        if self.memo_root:
            command += ['--memo', str(self.memo_root / role)]
        for trust in spec.get('trustcaches', []):
            command += ['--apple-trustcache', trust]
        if role == 'system_cryptex':
            for other in ('os', 'app_cryptex'):
                if other in self.roles:
                    command += ['--cdhash-pairs', str(self._pairs(other))]
        self.log.quiet('$', ' '.join(command))
        result = subprocess.run(command, capture_output=True, text=True)
        self.log.quiet(result.stdout + result.stderr)
        if result.returncode:
            tail = (result.stderr or result.stdout).strip().splitlines()[-1:] or ['?']
            raise host.HostError('translating {} failed: {}'.format(role, tail[0]))
        self._keep(role)
        entries = delta.load(self.out(role) / '{}.manifest.json'.format(role))
        self.log('{}: {} files to translate'.format(role, len(entries)))
        return entries

    def _pairs(self, role):
        return self.kept / '{}.cdhash-pairs'.format(role)

    def _keep(self, role):
        self.kept.mkdir(parents=True, exist_ok=True)
        for name in KEPT:
            path = self.out(role) / name.format(role=role)
            if path.is_file():
                shutil.copyfile(path, self.kept / path.name)

    def done(self, role):
        """The role's delta is applied: its replacement files go (the kept files stay for the boot package)."""
        if self.out(role).exists():
            shutil.rmtree(self.out(role))

    def artifacts(self):
        return sorted(self.kept.iterdir()) if self.kept.is_dir() else []

    def check_pins(self, pins):
        """The kept files against the ones the boot package was built with ({name: sha256}): a translation that made
        other trust modules or another cache list would not run on the kernel."""
        kept = {p.name: p for p in self.artifacts()}
        wrong = [
            name for name, digest in sorted(pins.items()) if name not in kept or cards.sha256_file(kept[name]) != digest
        ]
        if wrong:
            raise host.HostError(
                'this translation differs from the pinned one the boot package carries: {}'.format(', '.join(wrong))
            )
        self.log('pretranslate: {} files match the pins'.format(len(pins)))

    def overlay_system(self, data, system, entries, owners):
        """The System volume's translated files onto Data (OVERLAY), each with its System file's owner, mode and
        flags, then their manifest and the kernel's index made from it; the manifest is also kept for the boot
        package. As root, every file and directory of the tree ends system-immutable: the device's kernel refuses any
        change to the tree anyway, the flag is a second layer. Returns the manifest.

        Hard links of one System file get one Data file, placed at the first of their paths, and an index entry for
        each path. A link on Data would give the kernel's redirect a Data file whose parent changes with each lookup."""
        root = Path(data) / OVERLAY
        files, ids, placed = [], [], []
        by_inode = {}
        for e in entries:
            original = os.lstat(Path(system) / e['path'].lstrip('/'))
            if not stat.S_ISREG(original.st_mode):
                raise host.HostError('{}: not a regular file on the System volume'.format(e['path']))
            if original.st_ino in by_inode:
                first, data_id, record = by_inode[original.st_ino]
                if (first['sha256_out'], first.get('cdhash')) != (e['sha256_out'], e.get('cdhash')):
                    raise host.HostError(
                        '{} and {} are one System file translated two ways'.format(first['path'], e['path'])
                    )
                e['source'].unlink()
            else:
                target = _place(root, e, original, owners)
                placed.append(target)
                data_id = os.lstat(target).st_ino
                record = {
                    'trust': True,
                    'mode': '{:04o}'.format(original.st_mode & 0o7777),
                    'sha256': e['sha256_out'],
                    'cdhash': e.get('cdhash', []),
                }
                by_inode[original.st_ino] = (e, data_id, record)
            ids.append((e['path'], original.st_ino, data_id, _cdhash(e.get('cdhash') or [])))
            files.append({'path': e['path'], **record})
        manifest = {'files': files}
        text = json.dumps(manifest, indent=1) + '\n'
        root.mkdir(parents=True, exist_ok=True)
        (Path(data) / OVERLAY_MANIFEST).write_text(text)
        self.kept.mkdir(parents=True, exist_ok=True)
        (self.kept / 'os.overlay.json').write_text(text)
        directories = [root, *(p for p in root.rglob('*') if p.is_dir())]
        if owners:
            for directory in directories:
                os.chown(directory, 0, 0)
                directory.chmod(0o755)
        index = Path(data) / OVERLAY_INDEX
        index.write_bytes(overlay_index.encode(ids, [os.lstat(d).st_ino for d in directories]))
        index.chmod(0o644)
        if owners:
            os.chown(index, 0, 0)
            for path in [*placed, index, *reversed(directories)]:
                os.chflags(path, os.lstat(path).st_flags | stat.SF_IMMUTABLE)
        self.log('System: {} translated files on Data under {}'.format(len(files), OVERLAY))
        return manifest
