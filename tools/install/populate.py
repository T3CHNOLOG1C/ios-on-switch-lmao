"""What goes into the composed container's Data and Preboot volumes, mounted on the Mac as root (file owners).

Data     the System volume's /private/var (once), mobile's Preferences, the boot package's overlay, the Metal bundle
         (its stager, as //deploy:sd --stage-gpu) and the shader seed
Preboot  the two cryptex images, checked against the IPSW pins
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from tools.install import card as cards, host

SKIP = ('.fseventsd', '.Spotlight-V100', '.Trashes')


def seed_data(system: Path, data: Path, owners: bool, log: host.Log, reseed: bool = False) -> None:
    if (data / 'mobile').is_dir() and not reseed:
        log('data: already seeded (mobile/ exists); kept (--reseed-data replaces it)')
        return
    entries = [p for p in data.iterdir() if p.name not in SKIP]
    if entries:
        if not reseed:
            raise host.HostError('Data is not empty but has no mobile/: refusing to merge (use --reseed-data)')
        log('data: --reseed-data: clearing', len(entries), 'entries')
        for p in entries:
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p)
            else:
                p.unlink()
    host.run(
        'ditto',
        '--rsrc',
        '--extattr',
        '--acl',
        '--preserveHFSCompression',
        '--nocache',
        str(system / 'private/var'),
        str(data),
        log=log,
    )
    prefs = data / 'mobile/Library/Preferences'
    prefs.mkdir(parents=True, exist_ok=True)
    if owners:
        os.chown(data, 0, 0)
        data.chmod(0o755)
        for p in (data / 'mobile/Library', prefs):
            os.chown(p, 501, 501)
    log(
        'data: seeded from System /private/var ({} entries){}'.format(
            sum(1 for _ in data.rglob('*')), '' if owners else ' WITHOUT owners (unprivileged test: not bootable)'
        )
    )


def stage_cryptexes(
    preboot: Path,
    spec: dict,
    pins: dict,
    sources: dict[str, str | Path],
    owners: bool,
    log: host.Log,
    expected: dict[str, str] | None = None,
) -> None:
    """Stage the cryptex images into Preboot, each checked against its sha256.

    `sources`: {"system_cryptex": path, "app_cryptex": path}; `expected`: {member: sha256} for rebuilt images, which
    the IPSW pins do not describe.
    """
    expected = expected or {}
    for item in spec['cryptexes']:
        member = item['member']
        want = expected.get(member) or pins['members'][member]['content_sha256']
        relative = item['path'][len('/private/preboot/') :]
        source = Path(sources[member])
        if Path(relative).name != source.name:
            raise host.HostError('{}: the kernel opens {}, the image is {}'.format(member, item['path'], source))
        target = preboot / relative
        if target.is_file() and target.stat().st_size == source.stat().st_size and cards.sha256_file(target) == want:
            log(f'preboot: {relative} unchanged')
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name('.partial-' + target.name)
        host.copy_file_parallel(source, partial)
        if cards.sha256_file(partial) != want:
            partial.unlink()
            raise host.HostError(f'{relative}: copy does not match its sha256')
        os.replace(partial, target)
        target.chmod(0o644)
        if owners:
            os.chown(target.parent, 0, 0)
            os.chown(target, 0, 0)
        log(f'preboot: {relative} staged ({source.stat().st_size} bytes)')
    host.run('sync')


def stage_gpu(data: Path, stager: str, gpu_base: str, log: host.Log) -> None:
    result = subprocess.run(
        [sys.executable, stager, str(data), '--build', str(gpu_base)], capture_output=True, text=True, check=False
    )
    log('gpu:', (result.stdout + result.stderr).strip())
    if result.returncode:
        raise host.HostError('the Metal bundle was not staged')


def merge_tree(source: str | Path, target: Path, owners: bool, log: host.Log, what: str) -> None:
    """Copy `source` onto `target` (directories merged, files replaced): directories 0755, files 0644, root's."""
    count = 0
    for path in sorted(Path(source).rglob('*')):
        destination = Path(target) / path.relative_to(source)
        if path.is_dir():
            destination.mkdir(exist_ok=True)
            destination.chmod(0o755)
        else:
            shutil.copyfile(path, destination)
            destination.chmod(0o644)
            count += 1
        if owners:
            os.chown(destination, 0, 0)
    log(f'{what}: {count} files')
