"""iOS 27's user space, translated to plain arm64 while the image is composed (//translate/arm64e/pretranslate).

Each role's driver reads one mounted volume and writes its delta (delta.py) to a scratch directory. The SystemOS
cryptex goes last: the prebuilt loaders in its dyld cache check the binaries of every volume by cdhash, so its driver
takes the cdhash pairs the System volume's and the App cryptex's runs wrote, or each launch falls back to a slower
loader.
"""

import shutil
import subprocess
from pathlib import Path

from tools.install import delta, host

ORDER = ('os', 'app_cryptex', 'system_cryptex')
# What the boot package needs of a run (trust modules, the device tree's list of translated cache cdhashes).
KEPT = ('{role}.trustcache', '{role}.cdhash-pairs', 'a64x-cache-pretranslated.bin')


class Roles:
    def __init__(self, roles, scratch, log):
        """`roles`: {role: {"driver": path, "tool": path, "trustcaches": [path]}}."""
        unknown = set(roles) - set(ORDER)
        if unknown:
            raise host.HostError('unknown pretranslate roles: {}'.format(sorted(unknown)))
        self.roles, self.scratch, self.log = roles, Path(scratch), log
        self.kept = self.scratch / 'pretranslated'

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
