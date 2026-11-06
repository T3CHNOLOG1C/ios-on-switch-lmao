"""iOS 27's user space, translated to plain arm64 while the image is composed (//translate/arm64e/pretranslate).

Each role's driver reads one mounted volume and writes its delta (delta.py) to a scratch directory. The SystemOS
cryptex goes last: the prebuilt loaders in its dyld cache check the binaries of every volume by cdhash, so its driver
takes the cdhash pairs the System volume's and the App cryptex's runs wrote, or each launch falls back to a slower
loader.

The System volume stays sealed as Apple shipped it. Its translated files go to Data under Switch/pretranslated, at
their System paths, its own tree because a deploy replaces Switch/overlay whole; the kernel shows them at those paths
from Switch/pretranslated.index (overlay_index.py), which is made from Switch/pretranslated.json.
"""

import json
import os
import shutil
import subprocess
from pathlib import Path

from tools.install import card as cards, delta, host, overlay_index, renamed

OVERLAY = renamed.DATA_FOLDER + '/pretranslated'
OVERLAY_MANIFEST = OVERLAY + '.json'
OVERLAY_INDEX = OVERLAY + '.index'
COMPRESSED = 0x20

ORDER = ('os', 'app_cryptex', 'system_cryptex')
# What the boot package needs of a run (trust modules, the device tree's list of translated cache cdhashes).
KEPT = ('{role}.trustcache', '{role}.cdhash-pairs', 'a64x-cache-pretranslated.bin')


class Roles:
    def __init__(self, roles: dict[str, dict], scratch: Path, log: host.Log):
        """`roles`: {role: {"driver": path, "tool": path, "trustcaches": [path]}}."""
        unknown = set(roles) - set(ORDER)
        if unknown:
            raise host.HostError(f'unknown pretranslate roles: {sorted(unknown)}')
        self.roles, self.scratch, self.log = roles, Path(scratch), log
        self.kept = self.scratch / 'pretranslated'

    def __contains__(self, role: str) -> bool:
        return role in self.roles

    def out(self, role: str) -> Path:
        return self.scratch / ('delta-' + role)

    def run(self, role: str, volume: Path) -> list[dict]:
        """Make the role's delta for the volume mounted at `volume`; return its entries, ready for delta.apply."""
        if role == 'system_cryptex':
            missing = [r for r in ('os', 'app_cryptex') if r in self.roles and not self._pairs(r).is_file()]
            if missing:
                raise host.HostError('system_cryptex is translated after {}'.format(', '.join(missing)))
        spec = self.roles[role]
        command = [spec['driver'], '--role', role, '--tool', spec['tool'], '--volume', str(volume)]
        command += ['--out', str(self.out(role))]
        for trust in spec.get('trustcaches', []):
            command += ['--apple-trustcache', trust]
        if role == 'system_cryptex':
            for other in ('os', 'app_cryptex'):
                if other in self.roles:
                    command += ['--cdhash-pairs', str(self._pairs(other))]
        self.log.quiet('$', ' '.join(command))
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.log.quiet(result.stdout + result.stderr)
        if result.returncode:
            tail = (result.stderr or result.stdout).strip().splitlines()[-1:] or ['?']
            raise host.HostError(f'translating {role} failed: {tail[0]}')
        self._keep(role)
        entries = delta.load(self.out(role) / f'{role}.manifest.json')
        self.log(f'{role}: {len(entries)} files to translate')
        return entries

    def _pairs(self, role: str) -> Path:
        return self.kept / f'{role}.cdhash-pairs'

    def _keep(self, role: str) -> None:
        self.kept.mkdir(parents=True, exist_ok=True)
        for name in KEPT:
            path = self.out(role) / name.format(role=role)
            if path.is_file():
                shutil.copyfile(path, self.kept / path.name)

    def done(self, role: str) -> None:
        """Drop the role's delta once applied: its replacement files go (the kept files stay for the boot package)."""
        if self.out(role).exists():
            shutil.rmtree(self.out(role))

    def artifacts(self) -> list[Path]:
        return sorted(self.kept.iterdir()) if self.kept.is_dir() else []

    def check_pins(self, pins: dict[str, str]) -> None:
        """Check the kept files against the ones the boot package was built with ({name: sha256}).

        A translation that made other trust modules or another cache list would not run on the kernel.
        """
        kept = {p.name: p for p in self.artifacts()}
        wrong = [
            name for name, digest in sorted(pins.items()) if name not in kept or cards.sha256_file(kept[name]) != digest
        ]
        if wrong:
            raise host.HostError(
                'this translation differs from the pinned one the boot package carries: {}'.format(', '.join(wrong))
            )
        self.log(f'pretranslate: {len(pins)} files match the pins')

    def overlay_system(self, data: Path, system: Path, entries: list[dict], owners: bool) -> dict:
        """Put the System volume's translated files onto Data (OVERLAY), then their manifest and the kernel's index.

        Each file takes its System file's owner, mode and flags; the index is made from the manifest, which is also
        kept for the boot package. Returns the manifest.
        """
        root = Path(data) / OVERLAY
        files = []
        for e in entries:
            original = os.lstat(Path(system) / e['path'].lstrip('/'))
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
            files.append({
                'path': e['path'],
                'trust': True,
                'mode': f'{original.st_mode & 0o7777:04o}',
                'sha256': e['sha256_out'],
                'cdhash': e.get('cdhash', []),
            })  # fmt: skip
        manifest = {'files': files}
        text = json.dumps(manifest, indent=1) + '\n'
        root.mkdir(parents=True, exist_ok=True)
        (Path(data) / OVERLAY_MANIFEST).write_text(text)
        (Path(data) / OVERLAY_INDEX).write_bytes(overlay_index.encode([f['path'] for f in files]))
        self.kept.mkdir(parents=True, exist_ok=True)
        (self.kept / 'os.overlay.json').write_text(text)
        if owners:
            for directory in [root, *(p for p in root.rglob('*') if p.is_dir())]:
                os.chown(directory, 0, 0)
                directory.chmod(0o755)
        self.log(f'System: {len(files)} translated files on Data under {OVERLAY}')
        return manifest
