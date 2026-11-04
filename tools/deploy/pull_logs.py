#!/usr/bin/env python3
"""Pull the last boots' logs off the card's iPad Data volume (read-only but for harvest.py's one documented write).

    bazel run //deploy:pull_logs -- <diskN> [--out DIR] [--no-logarchive] [--no-harvest] [--no-csstore] [--no-launchd]
                                            [--no-eject]

Copies into <out> (default <workspace>/out/switch/device-logs/boot-<MMDD-HHMM>):
  diagnostics/, uuidtext/               the unified log (Data db/), and <out>/boot.logarchive built from them
                                        (`/usr/bin/log show --archive <out>/boot.logarchive --style compact --info --debug`)
  launchd.log                           (--no-launchd skips it)
  mobile-CrashReporter/, *.ips          crash reports, the WebKit ones also at the top level
  mobileactivationd/, migration-state.txt, boot-io-census.bin
  com.apple.LaunchServices-*.csstore + lsdb-webkit.txt   (--no-csstore skips them)
  m2v/ (+ m2v-cache -> backboardd's)    every process's m2v shader cache, whose journals are merged into the
                                        precompile corpus (tree/drivers/TegraGM20BMetal/precompile/corpus.log, a source
                                        file in your checkout) by that directory's harvest.py (--no-harvest skips it)
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.deploy import mkarchive  # noqa: E402
from tools.install import card as cards  # noqa: E402
from tools.install import host  # noqa: E402

DATA_UUID = "D1B1D053-D0A6-496C-8625-A947A3DA863E"   # IOSwitchRawMount.inc IOSW_RAW_DATA_UUID (checked against spec)
HARVEST = "out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/precompile"
WEBKIT_IDS = ("com.apple.WebKit.WebContent", "com.apple.WebKit.Networking", "com.apple.WebKit.GPU",
              "com.apple.WebKit.WebContent.CaptivePortal", "WebContentExtension.appex", "com.apple.fskit.exfat")
BACKBOARDD_CACHE = "m2v/System_6C7A65AF-CB18-421C-A07A-D724B05A7AB6_Library_Caches_m2v"


def workspace():
    return Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY") or Path(os.path.abspath(__file__)).parents[2])


def copytree(src, dst, log):
    if src.is_dir():
        shutil.copytree(src, dst, copy_function=shutil.copyfile, dirs_exist_ok=True)
        log.quiet("copied", src, "->", dst)
        return True
    return False


def pull(data, out, log, logarchive=True, harvest=True, csstore=True, launchd=True, corpus=None):
    data, out = Path(data), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    copytree(data / "db/diagnostics", out / "diagnostics", log)
    copytree(data / "db/uuidtext", out / "uuidtext", log)
    if launchd and (data / "log/com.apple.xpc.launchd/launchd.log").is_file():
        shutil.copyfile(data / "log/com.apple.xpc.launchd/launchd.log", out / "launchd.log")
    crash = data / "mobile/Library/Logs/CrashReporter"
    copytree(crash, out / "mobile-CrashReporter", log)
    for report in sorted(crash.glob("com.apple.WebKit.*.ips")) if crash.is_dir() else []:
        shutil.copyfile(report, out / report.name)
    copytree(data / "mobile/Library/Logs/mobileactivationd", out / "mobileactivationd", log)
    census = data / "ioswitch/boot-io-census.bin"
    if census.is_file():
        shutil.copyfile(census, out / census.name)
    lines = []
    for name in ("staged_system_apps", "datamigrator", "ioswitch"):
        path = data / name
        lines.append("{}: {}".format(name, sorted(p.name for p in path.iterdir()) if path.is_dir() else "missing"))
    apps = data / "containers/Bundle/Application"
    lines.append("installed apps: {}".format(sum(1 for _ in apps.iterdir()) if apps.is_dir() else 0))
    (out / "migration-state.txt").write_text("\n".join(lines) + "\n")
    if csstore:
        stores = sorted((data / "mobile/Containers/Data/InternalDaemon").glob(
            "*/Library/Caches/com.apple.LaunchServices-*.csstore"))
        if stores:
            counts = []
            for store in stores:
                shutil.copyfile(store, out / store.name)
            for ident in WEBKIT_IDS:
                total = sum(store.read_bytes().count(ident.encode()) for store in stores)
                counts.append("{} {}".format(ident, total))
            (out / "lsdb-webkit.txt").write_text("\n".join(counts) + "\n")
        else:
            log("LaunchServices database NOT copied (not found)")
    if harvest:
        script = workspace() / HARVEST / "harvest.py"
        corpus = Path(corpus or workspace() / HARVEST / "corpus.log")
        if script.is_file():
            result = subprocess.run([sys.executable, str(script), str(corpus), "--volume", str(data), "--save",
                                     str(out / "m2v")], capture_output=True, text=True)
            log.quiet(result.stdout + result.stderr)
            log("m2v journals {} into {}".format("harvested" if result.returncode == 0 else "NOT harvested", corpus))
            if (out / BACKBOARDD_CACHE).is_dir():
                link = out / "m2v-cache"
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(BACKBOARDD_CACHE)
        else:
            log("m2v harvest skipped: no", script)
    if logarchive and (out / "diagnostics/timesync").is_dir():
        try:
            boots = mkarchive.build(out, out / "boot.logarchive")
            log("logarchive: {} ({} boots)".format(out / "boot.logarchive", len(boots)))
        except SystemExit as error:
            log("logarchive NOT built:", error)
    log("logs ->", out)
    return out


def load_spec(path):
    """//install:spec from a runfiles path (bazel run) or a plain path."""
    import json
    if path and not os.path.isabs(path):
        try:
            from python.runfiles import runfiles
            path = runfiles.Create().Rlocation(path) or path
        except ImportError:
            pass
    if not path:
        return {"partition_bytes": 32 << 30, "volumes": {"data": {"uuid": DATA_UUID}}}
    return json.loads(Path(path).read_text())


def data_volume(card, spec_data_uuid=DATA_UUID):
    for container in host.apfs_containers():
        for volume in container.get("Volumes", []):
            if volume.get("APFSVolumeUUID", "").upper() == spec_data_uuid:
                store = (container.get("PhysicalStores") or [{}])[0].get("DeviceIdentifier", "")
                if store.startswith(card.disk + "s"):
                    return volume["DeviceIdentifier"]
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disk")
    parser.add_argument("--out")
    parser.add_argument("--corpus", help="precompile corpus to merge into (default: the checkout's corpus.log)")
    for flag in ("logarchive", "harvest", "csstore", "launchd"):
        parser.add_argument("--no-" + flag, action="store_true")
    parser.add_argument("--log")
    parser.add_argument("--spec", help="//install:spec (the kernel's volume UUIDs)")
    parser.add_argument("--no-eject", action="store_true", help="leave the card attached (default: unmount and eject)")
    args = parser.parse_args(argv)
    log = host.Log(args.log or Path.home() / "Library/Logs/ioswitch-install/pull-logs-{}.log".format(
        time.strftime("%Y%m%d-%H%M%S")))
    try:
        host.check_tools(log)
        spec = load_spec(args.spec)
        card = cards.resolve(args.disk)
        fat, target = cards.check_layout_unprivileged(card, spec)
        volume = data_volume(card, spec["volumes"]["data"]["uuid"])
        if not volume:
            raise host.HostError("no iPad Data volume {} on {}".format(spec["volumes"]["data"]["uuid"], card.disk))
        out = Path(args.out or workspace() / "out/switch/device-logs" / time.strftime("boot-%m%d-%H%M"))
        with cards.mounted(volume, log) as data:
            pull(data, out, log, logarchive=not args.no_logarchive, harvest=not args.no_harvest,
                 csstore=not args.no_csstore, launchd=not args.no_launchd, corpus=args.corpus)
        if not args.no_eject:
            # As deploy does: the iPad volumes unmounted, then the whole card ejected.
            container = host.container_on(target.identifier)
            if container:
                host.run("diskutil", "unmountDisk", container["ContainerReference"], log=log, check=False)
            host.run("diskutil", "eject", card.disk, log=log)
            log("ejected", card.disk)
        return 0
    except cards.Refused as error:
        log("REFUSED:", error)
        return 2
    except host.HostError as error:
        log("FAILED:", error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
