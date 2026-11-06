#!/usr/bin/env python3
"""Run exact exporter bytes in a Cortex-A57 model with synthetic firmware RAM."""

import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path
from types import ModuleType
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def tool(name: str) -> str:
    p = shutil.which(name)
    if p:
        return p
    for d in ('/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin'):
        if (Path(d) / name).is_file():
            return str(Path(d) / name)
    raise ValueError(name)


class QMP:
    def __init__(self, process: subprocess.Popen) -> None:
        self.input = process.stdin
        self.output = process.stdout
        self.seq = 0
        greeting = json.loads(self.output.readline())
        assert 'QMP' in greeting
        self.command('qmp_capabilities')

    def command(self, name: str, arguments: dict | None = None) -> object:
        self.seq += 1
        obj = {'execute': name, 'id': self.seq}
        if arguments is not None:
            obj['arguments'] = arguments
        self.input.write(json.dumps(obj).encode() + b'\n')
        self.input.flush()
        while True:
            obj = json.loads(self.output.readline())
            if obj.get('id') == self.seq:
                if 'error' in obj:
                    raise RuntimeError(obj['error'])
                return obj['return']

    def hmp(self, command: str) -> str:
        return self.command('human-monitor-command', {'command-line': command})


class _Case(NamedTuple):
    error: int
    directory: Path
    initial: bytes
    guard: bytes


def _caller(out: Path, fixture: ModuleType) -> int:
    source = '.arch armv8-a\n.section .text,"ax"\n.global _start\n_start:\n'
    source += '    ldr x9, =0x40220000\n    mov sp, x9\n'
    source += f'    ldr x18, ={fixture.GD:#x}\n'
    source += ''.join(f'    mov x{i}, #{i}\n' for i in range(19, 30))
    source += '    ldr x9, =0xa7000000\n    blr x9\n.global returned\nreturned:\n'
    source += '    ldr x9, =0x40210000\n    str x0, [x9, #8]\n    mov x10, sp\n    str x10, [x9, #16]\n'
    source += ''.join(f'    str x{i}, [x9, #{24 + (i - 18) * 8}]\n' for i in range(18, 31))
    source += (
        '    ldr x10, =0x45585054444f4e45\n    dsb sy\n    str x10, [x9]\n    dsb sy\nhold:\n    wfe\n    b hold\n'
    )
    (out / 'caller.S').write_text(source)
    subprocess.run(
        [
            tool('clang'),
            '--target=aarch64-none-elf',
            '-march=armv8-a',
            '-c',
            str(out / 'caller.S'),
            '-o',
            str(out / 'caller.o'),
        ],
        check=True,
    )
    subprocess.run(
        [tool('ld.lld'), '-Ttext=0x40200000', '--entry=_start', '-o', str(out / 'caller.elf'), str(out / 'caller.o')],
        check=True,
    )
    symbols = subprocess.check_output([tool('llvm-nm'), str(out / 'caller.elf')], text=True)
    return int(next(line.split()[0] for line in symbols.splitlines() if line.endswith(' returned')), 16)


def _firmware_ram(fixture: ModuleType, case: str) -> bytes:
    f = fixture.TestExport('test_exact_success_record')
    f.setUp()
    if case == 'unqualified-hc':
        f.p32(fixture.MMC + 0x1C, 0)
    if case == 'wrong-controller':
        f.p64(fixture.PRIVATE, 0x700B0600)
    if case == 'changed-layout-code':
        f.p32(fixture.BASE + 0x1C4B8, 0)
    if case == 'wrong-heap':
        f.p64(fixture.BASE + 0x66CB0, fixture.LOW + 8)
    return bytes(f.mem)


def _run(case: _Case, fixture: ModuleType, build: Path, out: Path) -> None:
    d, initial = case.directory, case.initial
    with tempfile.TemporaryDirectory(prefix='switch-fwe-'):
        cmd = [
            tool('qemu-system-aarch64'),
            '-machine',
            'virt,virtualization=on',
            '-cpu',
            'cortex-a57',
            '-m',
            '4096M',
            '-display',
            'none',
            '-serial',
            'none',
            '-monitor',
            'none',
            '-S',
            '-qmp',
            'stdio',
            '-device',
            f'loader,file={d / "firmware-ram.bin"},addr={fixture.GD:#x},force-raw=on',
            '-device',
            f'loader,file={d / "reference-before.bin"},addr=0xa001f000,force-raw=on',
            '-device',
            f'loader,file={build / "exporter.bin"},addr=0xa7000000,force-raw=on',
            '-device',
            f'loader,file={out / "caller.elf"},cpu-num=0',
        ]
        with (d / 'qemu.stderr').open('wb') as log:
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
            try:
                q = QMP(p)
                q.command('cont')
                for _ in range(100):
                    v = q.hmp('xp /1gx 0x40210000')
                    if '45585054444f4e45' in v:
                        break
                    time.sleep(0.02)
                else:
                    raise AssertionError('exporter failed to return: ' + q.hmp('info registers'))
                q.command('stop')
                for addr, n, name in [
                    (0x40210000, 128, 'caller-result.bin'),
                    (0xA001F000, 4096, 'reference-after.bin'),
                    (fixture.GD, len(initial), 'firmware-after.bin'),
                ]:
                    response = q.hmp(f'pmemsave {addr:#x} {n:#x} "{d / name}"')
                    assert not response, response
                (d / 'registers.txt').write_text(q.hmp('info registers'))
                q.command('quit')
                p.wait(timeout=5)
            finally:
                if p.poll() is None:
                    p.terminate()
                    p.wait(timeout=5)


def _record(case: _Case, fixture: ModuleType, returned: int, manifest: dict) -> bytes:
    error, d, initial, guard = case.error, case.directory, case.initial, case.guard
    result = (d / 'caller-result.bin').read_bytes()
    values = struct.unpack('<16Q', result)
    assert values[0] == 0x45585054444F4E45
    assert values[1] == error
    assert values[2] == 0x40220000
    assert values[3] == fixture.GD, values
    assert values[4:15] == tuple(range(19, 30)), values
    assert values[15] == returned, values
    assert (d / 'firmware-after.bin').read_bytes() == initial, 'firmware RAM changed'
    after = (d / 'reference-after.bin').read_bytes()
    record = after[0x400:0x500]
    if error:
        assert after[:0x400] == guard[:0x400]
        assert after[0x404:] == guard[0x404:]
        assert record[:4] == bytes(4)
    else:
        assert after[:0x400] == guard[:0x400]
        assert after[0x500:] == guard[0x500:]
        assert struct.unpack_from('<8I', record) == (0x4D435349, 1, 256, 1, 0x700B0000, 1, 512, 1)
        assert struct.unpack_from('<QQ', record, 0x20) == (fixture.CAPACITY, fixture.CAPACITY * 512)
        assert struct.unpack_from('<3I', record, 0x30) == (0xC0FF8000, 0xABCD, 0x80030000)
        assert struct.unpack_from('<4Q', record, 0x60) == (
            fixture.MMC,
            fixture.DESC,
            fixture.GD,
            fixture.BASE - 0x88110000,
        )
        assert record[0x40:0x60] == initial[fixture.MMC + 0x40 - fixture.GD : fixture.MMC + 0x60 - fixture.GD]
        assert record[0x80:0xA0] == bytes.fromhex(manifest['bl33_sha256'])
        assert record[0xA0:0xF0] == bytes(0x50)
        assert record[0xF4:] == bytes(12)
        assert struct.unpack_from('<I', record, 0xF0)[0] == zlib.crc32(record[:0xF0])
    return record


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    build = args.build.absolute()
    out = args.output.absolute()
    if out.exists() or any(p.is_symlink() for p in (out, *out.parents)):
        raise ValueError('new nonsymlink output required')
    if not out.is_relative_to(ROOT / 'out'):
        raise ValueError('workspace out required')
    manifest = json.loads((build / 'manifest.json').read_text())
    binary = (build / 'exporter.bin').read_bytes()
    assert hashlib.sha256(binary).hexdigest() == manifest['files']['exporter.bin']['sha256']
    out.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location('fixture', HERE / 'test_export.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    returned = _caller(out, fixture)
    reports = []
    for name, error in [
        ('success', 0),
        ('unqualified-hc', 8),
        ('wrong-controller', 7),
        ('changed-layout-code', 3),
        ('wrong-heap', 4),
    ]:
        d = out / name
        d.mkdir()
        initial = _firmware_ram(fixture, name)
        (d / 'firmware-ram.bin').write_bytes(initial)
        guard = bytes([0x5A]) * 4096
        (d / 'reference-before.bin').write_bytes(guard)
        case = _Case(error, d, initial, guard)
        _run(case, fixture, build, out)
        record = _record(case, fixture, returned, manifest)
        reports.append(
            {
                'case': name,
                'returned_error': error,
                'aapcs_x18_x19_to_x30_sp_preserved': True,
                'firmware_ram_unchanged': True,
                'reference_outside_allowed_writes_unchanged': True,
                'record_sha256': hashlib.sha256(record).hexdigest(),
            }
        )
    result = {
        'profile': 'exact-exporter-a57-synthetic-ram-model-v1',
        'exporter_sha256': hashlib.sha256(binary).hexdigest(),
        'build_manifest_sha256': hashlib.sha256((build / 'manifest.json').read_bytes()).hexdigest(),
        'cases': reports,
        'boundary': (
            'Exact AArch64 exporter executes against synthetic cached firmware RAM in generic QEMU Cortex-A57. '
            'Does not execute the real U-Boot image or exercise a physical card/controller, DMA, cache coherency, '
            'or Tegra hardware.'
        ),
    }
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
