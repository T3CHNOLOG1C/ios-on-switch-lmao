#!/usr/bin/env python3
"""Deploy a new build to an installed card (replaces out/switch/deploy-sd.sh). No sudo needed.

    bazel run //deploy:sd -- <diskN>                       dry run: check the card, show what would change
    bazel run //deploy:sd -- <diskN> --yes [--stage-gpu]   deploy (type the disk identifier to confirm)

Steps (each can be switched off):
  1 pull logs      the last boots' logs, logarchive and m2v journals (//deploy:pull_logs; --no-pull-logs skips it,
                   --no-csstore/--no-harvest/--no-launchd-log narrow it)
  2 boot entry     switchroot/ioswitch/* and bootloader/ini/ioswitch.ini on the FAT: only files that differ are
                   copied, stale files in switchroot/ioswitch/ removed, AppleDouble files by name; nothing else on the FAT
  3 overlay        <Data>/ioswitch/overlay := the package's overlay/; also removes the r3 Wi-Fi bring-up's configd Debug
                   logging profile (STALE_LOGGING_PROFILES) if a card still has it
  4 GPU bundle     --stage-gpu: <Data>/ngpu/addons/TegraGM20BMetal.{bundle,m2v-seed} from --//deploy:gpu_bundle, via
                   the plugin's stager (its CDHashes must be in the same build's kernel window: deploy the boot package
                   built with that bundle); --remove-gpu removes it
  5 launchd check  --launchd-check LABEL...: read-only, the jobs Data's launchd database disables (exit 1 if a label
                   is not among them; the kernel rewrites that database at every boot)
  6 eject          unmount the iPad volumes, eject the card (--no-eject keeps it)
"""
import argparse
import os
import plistlib
import subprocess
import sys
import time
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.deploy import pull_logs  # noqa: E402
from tools.install import card as cards  # noqa: E402
from tools.install import host  # noqa: E402


# Logging profiles an earlier bring-up put on Data by hand, which nothing else removes. 10-31 r3-wifi:
# com.apple.SystemConfiguration at Debug, persisted ("remove once Wi-Fi works"; Wi-Fi joins since r7j). On r7o it still
# wrote ~8 Debug lines/s to the persisted log (configd SCDynamicStore/IPMonitor, wifid SCPreferences).
STALE_LOGGING_PROFILES = ("preferences/Logging/Subsystems/com.apple.SystemConfiguration.plist",)


def remove_stale_logging_profiles(data_root, log):
    for name in STALE_LOGGING_PROFILES:
        path = Path(data_root) / name
        if path.is_file():
            path.unlink()
            log("removed stale logging profile", name)


def located(path):
    if path and not os.path.isabs(path):
        try:
            from python.runfiles import runfiles
            path = runfiles.Create().Rlocation(path) or path
        except ImportError:
            pass
    return os.path.abspath(path) if path else None


def launchd_check(data, labels, log):
    path = Path(data) / "db/com.apple.xpc.launchd/disabled.plist"
    entries = plistlib.loads(path.read_bytes()) if path.exists() else {}
    disabled = sorted(k for k, v in entries.items() if v is True or (isinstance(v, dict) and v.get("Disabled") is True))
    log("launchd: {} disables {}".format(path, disabled))
    if (path.parent / "disabled.501.plist").exists():
        log("launchd: disabled.501.plist is never read by launchd (no per-user database on iOS)")
    missing = [label for label in labels if label not in disabled]
    if missing:
        log("launchd: NOT disabled: {} (add them to native_ioswitch_watchdog_policy in kern_exec.c)".format(missing))
    return not missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disk")
    parser.add_argument("--yes", action="store_true")
    parser.add_argument("--confirm")
    parser.add_argument("--stage-gpu", action="store_true")
    parser.add_argument("--remove-gpu", action="store_true")
    parser.add_argument("--no-pull-logs", action="store_true")
    parser.add_argument("--no-csstore", action="store_true")
    parser.add_argument("--no-harvest", action="store_true")
    parser.add_argument("--no-launchd-log", action="store_true")
    parser.add_argument("--logs-out")
    parser.add_argument("--corpus")
    parser.add_argument("--launchd-check", nargs="+", metavar="LABEL", default=None)
    parser.add_argument("--no-eject", action="store_true")
    parser.add_argument("--boot-package", required=True)
    parser.add_argument("--spec", required=True)
    parser.add_argument("--gpu-bundle")
    parser.add_argument("--stager")
    parser.add_argument("--log")
    args = parser.parse_args(argv)
    for name in ("boot_package", "spec", "gpu_bundle", "stager"):
        setattr(args, name, located(getattr(args, name)))
    log = host.Log(args.log or Path.home() / "Library/Logs/ioswitch-install/deploy-{}.log".format(
        time.strftime("%Y%m%d-%H%M%S")))
    try:
        host.check_tools(log)
        spec = pull_logs.load_spec(args.spec)
        card = cards.resolve(args.disk)
        fat, target = cards.check_layout_unprivileged(card, spec)
        if target.content != "Apple_APFS":
            raise cards.Refused("{}: {} is not installed yet (run //install:sd first)".format(card.disk,
                                                                                            target.identifier))
        data_uuid = spec["volumes"]["data"]["uuid"]
        data = pull_logs.data_volume(card, data_uuid)
        if not data:
            raise cards.Refused("{}: no iPad Data volume {}".format(card.disk, data_uuid))
        if args.stage_gpu and not (args.gpu_bundle and (Path(args.gpu_bundle) / "TegraGM20BMetal.trustcache").is_file()):
            raise host.HostError("--stage-gpu: no GPU bundle (build //gpu:bundle; --//deploy:gpu_bundle=//gpu:bundle)")
        with cards.mounted(fat.identifier, log) as fat_root:
            changes, removed = cards.sync_boot_entry(fat_root, args.boot_package, log, dry_run=True)
        log("card {}: FAT {}, iPad Data {}".format(card.disk, fat.identifier, data))
        plan = (["pull logs"] if not args.no_pull_logs else []) + \
            ["boot entry: {} to copy, {} stale".format(len(changes), len(removed)), "overlay"] + \
            (["stage the GPU bundle"] if args.stage_gpu else []) + (["remove the GPU bundle"] if args.remove_gpu else []) + \
            (["launchd check {}".format(args.launchd_check)] if args.launchd_check else []) + \
            ([] if args.no_eject else ["unmount the iPad volumes and eject"])
        for step in plan:
            log("  plan:", step)
        if not args.yes:
            log("dry run: nothing written (add --yes)")
            return 0
        if args.confirm != card.disk:
            if args.confirm or not sys.stdin.isatty():
                raise cards.Refused("confirm with --confirm={} or interactively".format(card.disk))
            if input("Deploy to {}? Type {}: ".format(card.disk, card.disk)).strip() != card.disk:
                raise cards.Refused("not confirmed")
        ok = True
        with cards.mounted(data, log) as data_root:
            if not args.no_pull_logs:
                out = args.logs_out or pull_logs.workspace() / "out/switch/device-logs" / time.strftime(
                    "boot-%m%d-%H%M")
                pull_logs.pull(data_root, out, log, harvest=not args.no_harvest, csstore=not args.no_csstore,
                               launchd=not args.no_launchd_log, corpus=args.corpus)
            with cards.mounted(fat.identifier, log) as fat_root:
                cards.sync_boot_entry(fat_root, args.boot_package, log)
            cards.sync_overlay(data_root, args.boot_package, log)
            remove_stale_logging_profiles(data_root, log)
            if args.stage_gpu or args.remove_gpu:
                command = [sys.executable, args.stager, str(data_root)]
                command += ["--remove"] if args.remove_gpu else ["--build", args.gpu_bundle]
                result = subprocess.run(command, capture_output=True, text=True)
                log("gpu:", (result.stdout + result.stderr).strip())
                if result.returncode:
                    raise host.HostError("GPU bundle NOT staged")
            if args.launchd_check is not None:
                ok = launchd_check(data_root, args.launchd_check, log)
        if not args.no_eject:
            container = host.container_on(target.identifier)
            if container:
                host.run("diskutil", "unmountDisk", container["ContainerReference"], log=log, check=False)
            host.run("diskutil", "eject", card.disk, log=log)
            log("ejected", card.disk)
        log("deployed")
        return 0 if ok else 1
    except cards.Refused as error:
        log("REFUSED:", error)
        return 2
    except host.HostError as error:
        log("FAILED:", error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
