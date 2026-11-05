#!/usr/bin/env python3
"""Install iPadOS 17.7.11 (21H461) for ioswitch onto a Nintendo Switch SD card.

    bazel run //install:sd -- <diskN>                 dry run: check the card, print the plan, write nothing
    bazel run //install:sd -- <diskN> --yes           do it (asks you to type diskN, then runs itself under sudo)

Writes ONLY: the card's userdata partition (an APFS container: System, Data, Preboot), that partition's GPT entry
(type APFS, name IOSWITCH; primary and backup copies, nothing else in the partition table, never the hybrid MBR),
and on the FAT: switchroot/ioswitch/* and bootloader/ini/ioswitch.ini. See tools/install/card.py for the guards.

Steps, each skipped when the card already has its result (so a rerun only does what is missing or changed):
  1 compose  In a scratch image (not the card): the IPSW's OS container (System volume 04769D26...) grown to the
             partition's size, plus the Data and Preboot volumes, given the UUIDs the kernel was built with
             (diskutil cannot choose UUIDs; tools/install/apfs.py rewrites them offline), checked with fsck_apfs.
  2 write    Its written blocks onto the userdata partition, then the GPT entry's type to APFS.
  3 reprobe  macOS must re-read the card: a disk image is detached and attached again; a real card is ejected and
             you reinsert it and run the same command again (it continues at step 4).
  4 populate Data from the System volume's /private/var (only when Data is empty, or with --reseed-data); the two
             cryptex images into Preboot (by sha256 against the IPSW pins); the userland overlay.
  5 boot     The hekate entry on the FAT (only files that differ; stale files in switchroot/ioswitch removed).
  6 verify   One APFS partition of the right size, the partition table unchanged but for our entry, the volume UUIDs,
             fsck_apfs -n clean. Then the card is ejected (--no-eject keeps it).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.install import apfs, card as cards, gpt, host  # noqa: E402

COPY_CHUNK = 8 << 20
SEEK_HOLE, SEEK_DATA = getattr(os, "SEEK_HOLE", 3), getattr(os, "SEEK_DATA", 4)
F_PUNCHHOLE = 99          # <sys/fcntl.h>: deallocate a range of a file (APFS)


def punch(fd, offset, length):
    """Give a copied range of the scratch image back to the disk, so scratch + card never hold two copies."""
    import fcntl
    import struct
    try:
        fcntl.fcntl(fd, F_PUNCHHOLE, struct.pack("<IIqq", 0, 0, offset, length))
    except OSError:
        pass


def default_log():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return Path.home() / "Library/Logs/ioswitch-install/install-{}.log".format(stamp)


def resolve_inputs(args):
    """Runfiles paths (from `bazel run`) to absolute paths, so a sudo re-exec needs no Bazel environment."""
    try:
        from python.runfiles import runfiles
        locate = runfiles.Create()
    except ImportError:
        locate = None
    for name in ("spec", "ipsw", "os_dmg", "system_cryptex", "app_cryptex", "boot_package"):
        value = getattr(args, name)
        if value and not os.path.isabs(value) and locate:
            located = locate.Rlocation(value)
            if located:
                value = located
        if value:
            value = os.path.abspath(value)
        setattr(args, name, value)


class Installer:
    def __init__(self, args, log):
        self.args, self.log = args, log
        self.spec = json.loads(Path(args.spec).read_text())
        self.pins = json.loads(Path(args.ipsw).read_text())
        self.want = {k: v["uuid"] for k, v in self.spec["volumes"].items()}
        self.card = None

    def inspect(self):
        self.card = cards.resolve(self.args.disk)
        cards.read_table(self.card)
        cards.check_layout(self.card, self.spec)
        return self.state()

    def container_volumes(self):
        """[(uuid, role, name)] read from the target partition itself (works whether or not macOS attached it)."""
        fd = os.open(self.card.raw, os.O_RDONLY)
        try:
            return apfs.Container(fd, self.card.target.first * gpt.SECTOR).volumes()
        except apfs.ApfsError:
            return None
        finally:
            os.close(fd)

    def ours(self, found):
        have = {(str(u).upper(), role) for u, role, _ in (found or [])}
        return all((v["uuid"], v["role"]) in have for v in self.spec["volumes"].values())

    def state(self):
        target, found = self.card.target, self.container_volumes()
        if self.args.reinstall_container:
            # Never write under a container macOS has attached: make it Linux-typed again first (macOS detaches it on
            # the reprobe), then compose as on a fresh card.
            return "detach" if target.type == gpt.APFS else "compose"
        if target.type == gpt.APFS:
            if not self.ours(found):
                raise cards.Refused(
                    "{} is APFS but not an ioswitch container (volumes {}); rerun with --reinstall-container to "
                    "replace it".format(target.describe(), found))
            if not cards.volumes(self.card):
                return "reprobe"            # written, retyped, but macOS has not attached it yet
            return "populate"
        if self.ours(found):
            return "retype"                 # written by an interrupted run, still Linux-typed
        return "compose"

    def plan(self, state):
        steps = {
            "detach": ["retype the GPT entry back to Linux data so macOS lets go of the old container", "reprobe",
                       "compose", "write", "retype to APFS", "reprobe", "populate", "boot entry", "verify"],
            "compose": ["compose the container in {} (about {} GB free needed)".format(self.scratch_dir(), 7),
                        "write it to {} ({})".format(self.card.slice(self.card.target), self.card.target.describe()),
                        "retype the GPT entry to APFS, name IOSWITCH", "reprobe", "populate", "boot entry", "verify"],
            "retype": ["retype the GPT entry to APFS, name IOSWITCH", "reprobe", "populate", "boot entry", "verify"],
            "reprobe": ["reprobe", "populate", "boot entry", "verify"],
            "populate": ["populate (only what differs)", "boot entry", "verify"],
        }[state]
        return steps

    def scratch_dir(self):
        return Path(self.args.scratch or (Path.home() / "Library/Caches/ioswitch-install"))

    def attach_os(self):
        out = host.plist("hdiutil", "attach", "-plist", "-readonly", "-nomount", "-noverify", "-noautofsck",
                         self.args.os_dmg, log=self.log)
        apfs_part = [e["dev-entry"] for e in out["system-entities"]
                     if str(gpt.APFS).upper() in (e.get("content-hint", "").upper(),
                                                  e.get("unmapped-content-hint", "").upper())]
        devices = [e.get("dev-entry", "") for e in out["system-entities"]]
        if len(apfs_part) != 1:
            for d in devices:
                if d.startswith("/dev/disk") and "s" not in d[len("/dev/disk"):]:
                    subprocess.run([host.TOOLS["hdiutil"], "detach", d], capture_output=True)
            raise host.HostError("{}: expected one APFS partition, got {}".format(self.args.os_dmg, out))
        whole = apfs_part[0][:apfs_part[0].rindex("s")]
        return whole, apfs_part[0]

    def compose(self):
        scratch = self.scratch_dir()
        scratch.mkdir(parents=True, exist_ok=True)
        image = scratch / "compose.img"
        if image.exists():
            image.unlink()
        partition_sectors = self.spec["partition_sectors"]
        start = 2048
        disk_sectors = start + partition_sectors + 2048
        self.log("compose: attaching the IPSW's OS image", self.args.os_dmg)
        whole, source = self.attach_os()
        try:
            info = host.disk_info(source)
            os_bytes = int(info["TotalSize"])
            fd = os.open("/dev/r" + source[len("/dev/"):], os.O_RDONLY)
            try:
                found = apfs.Container(fd).volumes()
            finally:
                os.close(fd)
            if (self.want["system"], 1) not in {(str(u).upper(), r) for u, r, _ in found}:
                raise host.HostError("the OS image has no System volume {} ({})".format(self.want["system"], found))
            with open(image, "wb") as out:
                out.truncate(disk_sectors * gpt.SECTOR)
            ifd = os.open(image, os.O_RDWR)
            try:
                first_sectors = os_bytes // gpt.SECTOR
                for lba, data in gpt.build(disk_sectors, [(gpt.APFS, uuid.uuid4(), start, start + first_sectors - 1,
                                                           "compose")]):
                    os.pwrite(ifd, data, lba * gpt.SECTOR)
                sfd = os.open("/dev/r" + source[len("/dev/"):], os.O_RDONLY)
                try:
                    copied = 0
                    while copied < os_bytes:
                        chunk = os.pread(sfd, min(COPY_CHUNK, os_bytes - copied), copied)
                        if not chunk:
                            raise host.HostError("short read of the OS image at {}".format(copied))
                        if chunk.strip(b"\0"):      # zeros stay holes here; write() copies this range whole
                            os.pwrite(ifd, chunk, start * gpt.SECTOR + copied)
                        copied += len(chunk)
                finally:
                    os.close(sfd)
                os.fsync(ifd)
            finally:
                os.close(ifd)
            self.log("compose: OS container copied ({} bytes)".format(os_bytes))
        finally:
            host.run("hdiutil", "detach", whole, log=self.log)
        disk = self.attach_raw(image)
        try:
            store = disk + "s1"
            host.run("diskutil", "apfs", "resizeContainer", store, "{}B".format(self.spec["partition_bytes"]),
                     log=self.log)
            container = host.container_on(store)
            if not container:
                raise host.HostError("the scratch container did not attach")
            reference = container["ContainerReference"]
            for key in ("data", "preboot"):
                volume = self.spec["volumes"][key]
                role = {"data": "D", "preboot": "B"}[key]
                host.run("diskutil", "apfs", "addVolume", reference, "Case-sensitive APFS", volume["name"], "-role",
                         role, "-nomount", log=self.log)
            self.log("compose: grown to {} bytes, Data and Preboot added".format(self.spec["partition_bytes"]))
        finally:
            host.run("hdiutil", "detach", disk, log=self.log)
        # Offline: the volume UUIDs the kernel was built with.
        fd = os.open(image, os.O_RDWR)
        try:
            table = gpt.read(fd, os.fstat(fd).st_size)
            part = table.used[0]
            if part.sectors != partition_sectors:
                raise host.HostError("resizeContainer left the scratch partition at {} sectors, not {}".format(
                    part.sectors, partition_sectors))
            container = apfs.Container(fd, part.first * gpt.SECTOR)
            found = container.volumes()
            mapping = {}
            for key in ("data", "preboot"):
                spec = self.spec["volumes"][key]
                match = [u for u, role, name in found if role == spec["role"] and name == spec["name"]]
                if len(match) != 1:
                    raise host.HostError("scratch container: no single {} volume ({})".format(key, found))
                mapping[match[0]] = uuid.UUID(spec["uuid"])
            patched = container.set_volume_uuids(mapping)
            self.log("compose: volume UUIDs set offline in {} superblock copies: {}".format(
                len(patched), ", ".join("{}->{}".format(o, n) for _, o, n in patched)))
            got = {(str(u).upper(), r) for u, r, _ in container.volumes()}
            if not self.ours(container.volumes()):
                raise host.HostError("UUID rewrite did not take: {}".format(got))
            nx_blocks = int.from_bytes(container.superblocks()[0][40:48], "little")
            if nx_blocks * container.block_size != self.spec["partition_bytes"]:
                raise host.HostError("container is {} bytes, partition {}".format(nx_blocks * container.block_size,
                                                                                 self.spec["partition_bytes"]))
            part_offset = part.first * gpt.SECTOR
        finally:
            os.close(fd)
        disk = self.attach_raw(image)
        try:
            self.fsck(disk + "s1", "scratch")
        finally:
            host.run("hdiutil", "detach", disk, log=self.log)
        return image, part_offset, os_bytes

    def attach_raw(self, image):
        out = host.plist("hdiutil", "attach", "-plist", "-nomount", "-imagekey", "diskimage-class=CRawDiskImage",
                         str(image), log=self.log)
        disk = host.whole_disk_of(out["system-entities"])
        if not disk:
            raise host.HostError("hdiutil attach gave no disk for {}".format(image))
        return disk

    def fsck(self, store, what):
        result = subprocess.run([host.TOOLS["fsck_apfs"], "-n", "/dev/r" + store], capture_output=True, text=True)
        self.log.quiet(result.stdout + result.stderr)
        tail = (result.stdout + result.stderr).strip().splitlines()[-1:] or ["?"]
        if result.returncode or "appears to be OK" not in tail[0]:
            raise host.HostError("fsck_apfs -n {} ({}): {}".format(store, what, tail[0]))
        self.log("fsck_apfs {} ({}): {}".format(store, what, tail[0]))

    def write(self, image, part_offset, os_bytes):
        """The scratch partition's written regions onto the target partition. [0, os_bytes) is written whole (the
        OS container, checkpoint areas included, so nothing of an earlier container on the card survives there);
        beyond it only the extents the scratch file has data in (blocks the grown container wrote)."""
        target_offset = self.card.target.first * gpt.SECTOR
        size = self.spec["partition_bytes"]
        src = os.open(image, os.O_RDWR)
        dst = os.open(self.card.raw, os.O_RDWR)
        written = 0
        try:
            extents = [(0, os_bytes)]
            position = os_bytes
            while position < size:
                try:
                    data = os.lseek(src, part_offset + position, SEEK_DATA) - part_offset
                except OSError:
                    break
                if data >= size:
                    break
                hole = min(os.lseek(src, part_offset + data, SEEK_HOLE) - part_offset, size)
                extents.append((data, hole))
                position = hole
            for begin, end in extents:
                begin -= begin % 4096
                end += -end % 4096
                at = begin
                while at < min(end, size):
                    n = min(COPY_CHUNK, min(end, size) - at)
                    chunk = os.pread(src, n, part_offset + at)
                    if len(chunk) != n or os.pwrite(dst, chunk, target_offset + at) != n:
                        raise host.HostError("copy to {} failed at {}".format(self.card.raw, at))
                    punch(src, part_offset + at, n)     # the scratch shrinks as the card fills
                    at += n
                    written += n
            os.fsync(dst)
        finally:
            os.close(src)
            os.close(dst)
        self.log("write: {} bytes in {} extents onto {}".format(written, len(extents),
                                                                 self.card.slice(self.card.target)))

    def retype(self, kind=gpt.APFS, name="IOSWITCH"):
        fat = self.card.slice(self.card.fat)
        if host.mount_point(fat):
            host.run("diskutil", "unmount", fat, log=self.log)
        container = host.container_on(self.card.slice(self.card.target))
        if container:
            host.run("diskutil", "unmountDisk", container["ContainerReference"], log=self.log, check=False)
        before = cards.table_snapshot(self.card)
        writes = gpt.edit(self.card.table, self.card.target.index, kind, name)
        fd = os.open(self.card.raw, os.O_RDWR)
        try:
            gpt.apply(fd, writes)
        finally:
            os.close(fd)
        cards.read_table(self.card)
        after = cards.table_snapshot(self.card)
        if before[:gpt.SECTOR] != after[:gpt.SECTOR]:
            raise host.HostError("the MBR changed (it must not)")
        self.log("retype: {} -> {} {!r} ({} sectors rewritten)".format(self.card.target.describe(),
                                                                       gpt.NAMES[kind], name, len(writes)))

    def reprobe(self, expect_container=True):
        if self.card.image:
            image = self.card.image
            host.run("hdiutil", "detach", self.card.disk, log=self.log)
            fmt = host.run("hdiutil", "imageinfo", "-format", image).strip()
            args = ["attach", "-plist", "-nomount"]
            if fmt.startswith("RAW"):
                args += ["-imagekey", "diskimage-class=CRawDiskImage"]
            out = host.plist("hdiutil", *args, image, log=self.log)
            disk = host.whole_disk_of(out["system-entities"])
            self.log("reprobe: image re-attached as", disk)
            self.args.disk = disk
            for _ in range(30):
                self.inspect()
                if bool(cards.volumes(self.card)) == expect_container:
                    return True
                time.sleep(1)
            raise host.HostError("macOS did {}attach the container on {}".format("not " if expect_container else "",
                                                                                 disk))
        host.run("diskutil", "eject", self.card.disk, log=self.log)
        self.log("\nThe card is ejected so macOS reads its new partition table. Reinsert it and run the same command "
                 "again; it continues where it stopped.")
        return False

    def populate(self):
        vols = cards.volumes(self.card)
        missing = [k for k, u in self.want.items() if u not in vols]
        if missing:
            raise host.HostError("volumes {} not attached (have {})".format(missing, sorted(vols)))
        dev = {k: vols[u]["DeviceIdentifier"] for k, u in self.want.items()}
        owners = os.geteuid() == 0
        if not owners and not self.args.unprivileged_test:
            raise host.HostError("populating needs root (file owners): run with sudo")
        with cards.mounted(dev["system"], self.log, read_only=True) as system, \
                cards.mounted(dev["data"], self.log, owners=True) as data, \
                cards.mounted(dev["preboot"], self.log, owners=True) as preboot:
            if not (system / "private/preboot").is_dir():
                raise host.HostError("the System volume has no /private/preboot (the kernel mounts Preboot there)")
            self.seed_data(system, data, owners)
            self.stage_cryptexes(preboot, owners)
            if self.args.boot_package:
                cards.sync_overlay(data, self.args.boot_package, self.log)

    def seed_data(self, system, data, owners):
        if (data / "mobile").is_dir() and not self.args.reseed_data:
            self.log("data: already seeded (mobile/ exists); kept (--reseed-data replaces it)")
            return
        entries = [p for p in data.iterdir() if p.name not in (".fseventsd", ".Spotlight-V100", ".Trashes")]
        if entries:
            if not self.args.reseed_data:
                raise host.HostError("Data is not empty but has no mobile/: refusing to merge (use --reseed-data)")
            self.log("data: --reseed-data: clearing", len(entries), "entries")
            for p in entries:
                if p.is_dir() and not p.is_symlink():
                    shutil.rmtree(p)
                else:
                    p.unlink()
        host.run("ditto", "--rsrc", "--extattr", "--acl", "--preserveHFSCompression", "--nocache",
                 str(system / "private/var"), str(data), log=self.log)
        prefs = data / "mobile/Library/Preferences"
        prefs.mkdir(parents=True, exist_ok=True)
        if owners:
            os.chown(data, 0, 0)
            data.chmod(0o755)
            for p in (data / "mobile/Library", prefs):
                os.chown(p, 501, 501)
        count = sum(1 for _ in data.rglob("*"))
        self.log("data: seeded from System /private/var ({} entries){}".format(
            count, "" if owners else " WITHOUT owners (unprivileged test: not bootable)"))

    def stage_cryptexes(self, preboot, owners):
        for item in self.spec["cryptexes"]:
            pin = self.pins["members"][item["member"]]
            relative = item["path"][len("/private/preboot/"):]
            if Path(relative).name != Path(pin["member"]).name:
                raise host.HostError("{}: the kernel opens {}, the IPSW member is {}".format(
                    item["member"], item["path"], pin["member"]))
            target = preboot / relative
            source = Path(getattr(self.args, item["member"]))
            if target.is_file() and target.stat().st_size == pin["size"] and \
                    cards.sha256_file(target) == pin["content_sha256"]:
                self.log("preboot: {} unchanged".format(relative))
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(".partial-" + target.name)
            shutil.copyfile(source, partial)
            if cards.sha256_file(partial) != pin["content_sha256"]:
                partial.unlink()
                raise host.HostError("{}: copy does not match the pinned sha256".format(relative))
            os.replace(partial, target)
            target.chmod(0o644)
            if owners:
                os.chown(target.parent, 0, 0)
                os.chown(target, 0, 0)
            self.log("preboot: {} staged ({} bytes)".format(relative, pin["size"]))
        host.run("sync")

    def boot_entry(self):
        if not self.args.boot_package:
            self.log("boot entry: no boot package given; skipped")
            return
        fat = self.card.slice(self.card.fat)
        with cards.mounted(fat, self.log) as root:
            cards.sync_boot_entry(root, self.args.boot_package, self.log)

    def verify(self, snapshot, retyped):
        self.inspect()
        after = cards.table_snapshot(self.card)
        if len(after) != len(snapshot):
            raise host.HostError("verify: the partition table changed shape")
        table, span = self.card.table, self.card.table.entry_sectors * gpt.SECTOR
        allowed = set()
        if retyped:     # the primary header, the target entry in both arrays, the backup header
            entry = self.card.target.index * table.entry_size
            allowed |= set(range(gpt.SECTOR, 2 * gpt.SECTOR))
            for base in (2 * gpt.SECTOR, 2 * gpt.SECTOR + span):
                allowed |= set(range(base + entry, base + entry + table.entry_size))
            allowed |= set(range(len(after) - gpt.SECTOR, len(after)))
        changed = [i for i in range(len(after)) if after[i] != snapshot[i] and i not in allowed]
        if changed:
            raise host.HostError("verify: partition-table bytes changed outside the target entry: offsets {}".format(
                changed[:16]))
        self.log("verify: partition table: {} (MBR untouched)".format(
            "only the target entry and the two headers changed" if retyped else "unchanged"))
        apfs_entries = [e for e in self.card.table.used if e.type == gpt.APFS]
        if len(apfs_entries) != 1 or apfs_entries[0].sectors != self.spec["partition_sectors"]:
            raise host.HostError("verify: APFS entries {}".format([e.describe() for e in apfs_entries]))
        vols = cards.volumes(self.card)
        for key, want in self.want.items():
            if want not in vols:
                raise host.HostError("verify: volume {} {} missing".format(key, want))
        self.fsck(self.card.slice(self.card.target), "card")
        self.log("verify: one APFS partition ({} bytes), volumes {}".format(
            self.spec["partition_bytes"], ", ".join("{} {}".format(k, v) for k, v in self.want.items())))

    def run(self):
        try:
            state = self.inspect()
        except host.HostError as error:
            if "needs root" not in str(error) or self.args.yes:
                raise
            card = cards.resolve(self.args.disk)
            fat, target = cards.check_layout_unprivileged(card, self.spec)
            self.log("card: {} ({} bytes); FAT {}, target {} {!r} ({} bytes, {})".format(
                card.disk, card.size, fat.identifier, target.identifier, target.name, target.size, target.content))
            self.log("dry run: the layout passes diskutil's checks. Reading the partition table itself (hybrid MBR, "
                     "both GPT copies) and the container needs root: --yes runs the full checks under sudo before "
                     "writing anything.")
            return 0
        self.log("card: {} ({} bytes{}), target {}".format(
            self.card.disk, self.card.size, ", image " + self.card.image if self.card.image else "",
            self.card.target.describe()))
        self.log("state:", state)
        for step in self.plan(state):
            self.log("  plan:", step)
        if not self.args.yes:
            self.log("dry run: nothing written (add --yes to install)")
            return 0
        snapshot = cards.table_snapshot(self.card)
        backup = Path(str(self.log.path) + ".partition-table.bin")
        backup.write_bytes(snapshot)
        self.log("partition table backed up to", backup)
        retyped = False
        if state == "detach":
            self.retype(gpt.LINUX_DATA, "userdata")
            retyped = True
            if not self.reprobe(expect_container=False):
                return 0
            snapshot = cards.table_snapshot(self.card)
            retyped = False
            state = "compose"
        if state == "compose":
            if host.container_on(self.card.slice(self.card.target)):
                raise host.HostError("macOS has a container attached on {}: refusing to write under it".format(
                    self.card.slice(self.card.target)))
            image, offset, os_bytes = self.compose()
            try:
                self.write(image, offset, os_bytes)
            finally:
                if not self.args.keep_scratch:
                    image.unlink()
            state = "retype"
        if state == "retype":
            self.retype()
            retyped = True
            state = "reprobe"
        if state == "reprobe":
            if not self.reprobe():
                return 0
        self.populate()
        self.boot_entry()
        self.verify(snapshot, retyped)
        if not self.card.image and not self.args.no_eject:
            host.run("diskutil", "eject", self.card.disk, log=self.log)
            self.log("ejected", self.card.disk)
        self.log("done")
        return 0


def confirm(args, card_disk):
    if args.confirm:
        if args.confirm != card_disk:
            raise cards.Refused("--confirm={} does not name the target {}".format(args.confirm, card_disk))
        return
    if not sys.stdin.isatty():
        raise cards.Refused("type-to-confirm needs a terminal (or pass --confirm={})".format(card_disk))
    answer = input("This rewrites {}'s userdata partition. Type {} to continue: ".format(card_disk, card_disk))
    if answer.strip() != card_disk:
        raise cards.Refused("not confirmed")
    args.confirm = card_disk


def reexec_as_root(args, argv):
    """After confirmation: the same command under sudo, with absolute input paths and the confirmation."""
    command = ["sudo", "--", sys.executable, os.path.abspath(__file__)]
    passthrough = [a for a in argv if not a.startswith("--confirm")]
    for name in ("spec", "ipsw", "os_dmg", "system_cryptex", "app_cryptex", "boot_package", "scratch", "log"):
        value = getattr(args, name)
        flag = "--" + name.replace("_", "-")
        passthrough = [a for a in passthrough if not a.startswith(flag + "=")]
        if value:
            passthrough.append("{}={}".format(flag, value))
    command += passthrough + ["--confirm={}".format(args.confirm)]
    print("running under sudo:", " ".join(command), flush=True)
    os.execvp("sudo", command)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disk", help="the card's whole disk, e.g. disk4 (see `diskutil list external`)")
    parser.add_argument("--yes", action="store_true", help="install (default: dry run)")
    parser.add_argument("--confirm", help="the disk identifier, instead of typing it (scripts and tests)")
    parser.add_argument("--reseed-data", action="store_true", help="replace Data's contents with a fresh seed")
    parser.add_argument("--reinstall-container", action="store_true", help="rewrite the container even if present")
    parser.add_argument("--no-eject", action="store_true")
    parser.add_argument("--keep-scratch", action="store_true")
    parser.add_argument("--unprivileged-test", action="store_true",
                        help="disk images you own only: run without root, skipping file ownership (NOT bootable)")
    parser.add_argument("--scratch", help="scratch directory for the composed container (default ~/Library/Caches/"
                                          "ioswitch-install; needs about 7 GB free)")
    parser.add_argument("--log")
    parser.add_argument("--spec", required=True)
    parser.add_argument("--ipsw", required=True)
    parser.add_argument("--os-dmg", required=True)
    parser.add_argument("--system-cryptex", required=True)
    parser.add_argument("--app-cryptex", required=True)
    parser.add_argument("--boot-package")
    args = parser.parse_args(argv)
    resolve_inputs(args)
    args.log = args.log or str(default_log())
    log = host.Log(args.log)
    try:
        host.check_tools(log)
        installer = Installer(args, log)
        if args.yes:
            card = cards.resolve(args.disk)
            if args.unprivileged_test and not card.image:
                raise cards.Refused("--unprivileged-test is only for disk images")
            confirm(args, card.disk)
            if os.geteuid() != 0 and not args.unprivileged_test:
                args.scratch = str(installer.scratch_dir())
                reexec_as_root(args, argv)
        return installer.run()
    except cards.Refused as error:
        log("REFUSED:", error)
        return 2
    except (host.HostError, apfs.ApfsError, gpt.GptError) as error:
        log("FAILED:", error)
        return 1
    finally:
        log.quiet("log:", args.log)


if __name__ == "__main__":
    sys.exit(main())
