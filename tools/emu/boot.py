"""Boot this workspace's kernel on the emulated Switch and write a run directory (README.md).

    bazel run //tools/emu:boot -- --kind=ios17|ios27 [--sd IMAGE|none] [--seconds N] [--bootargs ARGS]

Builds //:boot for the kind in this workspace and output base (--bazel-flag passes flags to that build), gets the
patched tegra_qemu from the shared cache, boots a clone of the shared SD card for N seconds of wall time, stops the
machine and saves what it shows. The run directory holds the UART log, the kernel log, the panic, the screen and
summary.txt.
"""

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from tools.emu import machine, qemu, report, sdcard
from tools.emu.monitor import Monitor, socket_path
from tools.install import stages

KINDS = {'ios17': '21H461', 'ios27': '24B119'}


def workspace() -> Path:
    return Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY', os.getcwd()))


def output_base() -> Path:
    runfiles = os.environ.get('RUNFILES_DIR') or os.path.abspath(sys.argv[0])
    if '/execroot/' not in runfiles:
        sys.exit('run this through `bazel run //tools/emu:boot`, or give --package')
    return Path(runfiles.split('/execroot/')[0])


def bazel(base: Path, *args: str) -> str:
    command = ['bazel', f'--output_base={base}', *args]
    result = subprocess.run(command, cwd=workspace(), stdout=subprocess.PIPE, check=False, text=True)
    if result.returncode:
        sys.exit(f'{" ".join(command)} failed')
    return result.stdout.strip()


def build_package(kind: str, flags: list[str]) -> Path:
    base = output_base()
    options = [f'--//ipsw:kind={KINDS[kind]}', *flags]
    bazel(base, 'build', *options, '//:boot')
    relative = bazel(base, 'cquery', *options, '--output=files', '//:boot')
    return Path(bazel(base, 'info', *options, 'execution_root')) / relative


def default_container(kind: str) -> Path | None:
    scratch = stages.default_scratch(KINDS[kind])
    for name in (stages.POPULATED, stages.COMPOSED):
        if (scratch / name).exists():
            return scratch / name
    return None


def card_files(package: Path, run: Path, bootargs: str | None) -> dict[str, Path]:
    files = {
        str(path.relative_to(package)): path
        for top in ('bootloader', 'switchroot')
        for path in (package / top).rglob('*')
        if path.is_file()
    }
    if bootargs is not None:
        (run / 'bootargs.txt').write_text(bootargs + '\n')
        files['switchroot/ios/bootargs.txt'] = run / 'bootargs.txt'
    return files


def regwrite() -> Path:
    return Path(__file__).parent / 'regwrite.bin'


def run_machine(run: Path, inputs: machine.Inputs, seconds: float) -> None:
    command = machine.command(run, inputs)
    with (run / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, cwd=run, stdout=log, stderr=subprocess.STDOUT)
    try:
        monitor = Monitor(inputs.qmp)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.5)
        if process.poll() is None:
            report.capture(monitor, run)
            with contextlib.suppress(OSError, RuntimeError):
                monitor.command('quit')
        monitor.close()
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
        shutil.rmtree(inputs.qmp.parent, ignore_errors=True)


def parse() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kind', choices=sorted(KINDS), required=True)
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--sd', help="the APFS container image (tools/install's composed or populated.img), or none")
    parser.add_argument('--bootargs', help='switchroot/ios/bootargs.txt for this run, e.g. -v')
    parser.add_argument('--out', type=Path, help='the run directory (default out/emu/<kind>-<time>)')
    parser.add_argument('--package', type=Path, help='a //:boot output to boot instead of building one')
    parser.add_argument('--bazel-flag', action='append', default=[], help='a flag for the //:boot build')
    parser.add_argument('--qemu', type=Path, help='this qemu-system-aarch64 instead of the cached build')
    parser.add_argument('--keep-sd', action='store_true', help="keep the run's card (an APFS clone)")
    return parser.parse_args()


def main() -> None:
    args = parse()
    package = args.package.resolve() if args.package else build_package(args.kind, args.bazel_flag)
    binary = args.qemu.resolve() if args.qemu else qemu.ensure()
    run = (args.out or workspace() / 'out/emu' / f'{args.kind}-{time.strftime("%Y%m%d-%H%M%S")}').resolve()
    run.mkdir(parents=True, exist_ok=True)
    container = None if args.sd == 'none' else Path(args.sd) if args.sd else default_container(args.kind)
    card = sdcard.card_for_run(container, run, card_files(package, run, args.bootargs))
    inputs = machine.Inputs(qemu=binary, package=package, regwrite=regwrite(), sd=card, qmp=socket_path())
    try:
        run_machine(run, inputs, args.seconds)
    finally:
        if not args.keep_sd:
            card.unlink(missing_ok=True)
    print(report.summarize(run), end='')


if __name__ == '__main__':
    main()
