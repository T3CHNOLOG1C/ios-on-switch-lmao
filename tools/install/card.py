"""The Switch SD card as the installer and deploy see it: guards, layout, volumes, and the two places we write.

Guard rails (every command):
  * the argument must name a whole disk (diskN or /dev/diskN), never a partition, volume or path;
  * never the boot disk or a disk holding the running system's APFS container;
  * never an internal non-removable disk;
  * it must look like a Switch card: valid primary and backup GPT, entry #0 "hos_data" a Microsoft basic data (FAT32)
    partition that the hybrid MBR also lists;
  * the target is exactly one entry whose partition GUID is the card's userdata GUID or whose name is userdata, UDA or
    IOSWITCH, exactly the kernel's IOSW_RAW_BYTES (32 GiB), Linux-data or APFS typed, not the FAT, not emuMMC;
  * no other APFS-typed entry on the card (the kernel takes the only one).
Writes on the FAT go only to switchroot/ioswitch/* and bootloader/ini/ioswitch.ini (by name); on the iPad volumes
only to the paths the caller names.
"""
import contextlib
import hashlib
import os
import shutil
import tempfile
import uuid
from dataclasses import dataclass
from pathlib import Path

from tools.install import gpt, host

USERDATA_GUID = uuid.UUID("5703F8E9-0032-DF6B-2674-B1A569FFC4EE")
TARGET_NAMES = {"userdata", "UDA", "IOSWITCH"}
BOOT_ENTRY = "switchroot/ioswitch"
INI = "bootloader/ini/ioswitch.ini"
HASH_CHUNK = 8 << 20


class Refused(Exception):
    """A guard said no. The message says which and why; nothing was written."""


@dataclass
class Card:
    disk: str                  # disk4
    info: dict
    size: int
    image: str = ""            # attached disk image path ("" for real media)
    table: gpt.Table = None    # None until read (raw read needs root on real cards)
    target: gpt.Entry = None
    fat: gpt.Entry = None

    @property
    def raw(self):
        return "/dev/r" + self.disk

    def slice(self, entry):
        return "{}s{}".format(self.disk, entry.index + 1)


def resolve(argument):
    """The whole disk named by `argument`, after the host-level guards (no raw access needed)."""
    name = argument[len("/dev/"):] if argument.startswith("/dev/") else argument
    if name.startswith("r"):
        name = name[1:]
    if not name.startswith("disk") or not name[4:].isdigit():
        raise Refused("{!r} is not a whole disk (expected diskN)".format(argument))
    info = host.disk_info(name)
    if not info:
        raise Refused("no disk {}".format(name))
    if info.get("ParentWholeDisk") != name or not info.get("WholeDisk"):
        raise Refused("{} is not a whole disk".format(name))
    if info.get("APFSPhysicalStores") or info.get("Content") == "EF57347C-0000-11AA-AA11-00306543ECAC":
        raise Refused("{} is an APFS container (synthesized), not the card: name the card's own disk".format(name))
    if info.get("DeviceBlockSize", 512) != 512:
        raise Refused("{} has {}-byte blocks; an SD card has 512".format(name, info.get("DeviceBlockSize")))
    if name in host.boot_disks():
        raise Refused("{} holds the running system".format(name))
    image = host.images().get(name, "")
    if info.get("Internal") and not info.get("RemovableMedia") and not image:
        raise Refused("{} is an internal, non-removable disk".format(name))
    if info.get("BusProtocol") == "Disk Image" and not image:
        raise Refused("{} is a disk image hdiutil does not list".format(name))
    if not info.get("WritableMedia", True):
        raise Refused("{} is read-only media (SD lock switch?)".format(name))
    return Card(name, info, int(info["TotalSize"]), image)


def read_table(card):
    try:
        fd = os.open(card.raw, os.O_RDONLY)
    except PermissionError:
        raise host.HostError("reading {}'s partition table needs root (sudo)".format(card.raw))
    try:
        card.table = gpt.read(fd, card.size)
    except gpt.GptError as error:
        raise Refused("{}: {}".format(card.disk, error))
    finally:
        os.close(fd)
    return card.table


def check_layout(card, spec):
    """Find hos_data and the target, or refuse."""
    table = card.table
    entries = table.used
    first = table.entries[0]
    if first.type != gpt.BASIC_DATA or first.name != "hos_data":
        raise Refused("{}: entry #0 is {}, not the Switch's hos_data FAT32 partition".format(card.disk,
                                                                                           first.describe()))
    if not any(p.kind in (0x0B, 0x0C) and p.first == first.first for p in table.mbr_partitions):
        raise Refused("{}: the MBR does not list hos_data as FAT32 (no hybrid MBR: not a Switch card?)".format(
            card.disk))
    fat_info = host.disk_info(card.slice(first)) or {}
    if fat_info.get("FilesystemType") not in ("msdos", None) and fat_info.get("FilesystemType"):
        raise Refused("{}: hos_data is {}, not FAT".format(card.disk, fat_info.get("FilesystemType")))
    candidates = [e for e in entries if e.guid == USERDATA_GUID or e.name in TARGET_NAMES]
    if len(candidates) != 1:
        raise Refused("{}: expected exactly one userdata/UDA/IOSWITCH entry, found {}".format(
            card.disk, [e.describe() for e in candidates]))
    target = candidates[0]
    if target.index == first.index or target.type == gpt.EMUMMC:
        raise Refused("{}: the target {} is the FAT or emuMMC".format(card.disk, target.describe()))
    if target.sectors != spec["partition_sectors"]:
        raise Refused("{}: {} is {} bytes; the kernel requires exactly {} (IOSW_RAW_BYTES)".format(
            card.disk, target.describe(), target.bytes, spec["partition_bytes"]))
    if target.type not in (gpt.LINUX_DATA, gpt.APFS):
        raise Refused("{}: {} is neither Linux data nor APFS".format(card.disk, target.describe()))
    others = [e for e in entries if e.type == gpt.APFS and e.index != target.index]
    if others:
        raise Refused("{}: other APFS partitions {}: the kernel needs exactly one".format(
            card.disk, [e.describe() for e in others]))
    if any(target.first <= p.first + p.sectors - 1 and p.first <= target.last
           for p in table.mbr_partitions if p.kind not in (0, 0xEE)):
        raise Refused("{}: the hybrid MBR lists a partition overlapping the target".format(card.disk))
    card.target, card.fat = target, first
    return target


@dataclass
class Part:
    """A partition as diskutil reports it (no raw access needed)."""
    index: int
    identifier: str
    name: str
    guid: str
    content: str
    size: int
    filesystem: str


def diskutil_partitions(card):
    out = []
    for item in host.partitions(card.disk):
        ident = item["DeviceIdentifier"]
        info = host.disk_info(ident) or {}
        out.append(Part(int(ident.rsplit("s", 1)[1]) - 1, ident, info.get("IORegistryEntryName", ""),
                        (info.get("DiskUUID") or "").upper(), info.get("Content", ""), int(info.get("TotalSize", 0)),
                        info.get("FilesystemType") or ""))
    return out


def check_layout_unprivileged(card, spec):
    """The same guards as check_layout, from diskutil's view (deploy, and dry runs without root). Returns (fat
    partition, target partition)."""
    parts = diskutil_partitions(card)
    if not parts or parts[0].name != "hos_data" or parts[0].content != "Microsoft Basic Data" or \
            parts[0].filesystem not in ("msdos", ""):
        raise Refused("{}: the first partition is not the Switch's hos_data FAT32".format(card.disk))
    targets = [p for p in parts if p.guid == str(USERDATA_GUID).upper() or p.name in TARGET_NAMES]
    if len(targets) != 1:
        raise Refused("{}: expected exactly one userdata/UDA/IOSWITCH partition, found {}".format(
            card.disk, [p.identifier for p in targets]))
    target = targets[0]
    if target.size != spec["partition_bytes"]:
        raise Refused("{}: {} is {} bytes, not {}".format(card.disk, target.identifier, target.size,
                                                         spec["partition_bytes"]))
    apfs_parts = [p for p in parts if p.content == "Apple_APFS" and p.identifier != target.identifier]
    if apfs_parts:
        raise Refused("{}: other APFS partitions {}".format(card.disk, [p.identifier for p in apfs_parts]))
    return parts[0], target


def table_snapshot(card):
    """The bytes of every partition-table sector (MBR, both headers and arrays) for before/after checks."""
    fd = os.open(card.raw, os.O_RDONLY)
    try:
        t = card.table
        span = t.entry_sectors
        parts = [os.pread(fd, (2 + span) * gpt.SECTOR, 0),
                 os.pread(fd, span * gpt.SECTOR, t.backup_entries_lba * gpt.SECTOR),
                 os.pread(fd, gpt.SECTOR, (t.disk_sectors - 1) * gpt.SECTOR)]
    finally:
        os.close(fd)
    return b"".join(parts)


def volumes(card):
    """{uuid: {DeviceIdentifier, Roles, Name, ...}} of the APFS container on the target, {} when none is attached."""
    container = host.container_on(card.slice(card.target))
    if not container:
        return {}
    return {v["APFSVolumeUUID"].upper(): v for v in container.get("Volumes", [])}


@contextlib.contextmanager
def mounted(identifier, log, read_only=False, owners=False):
    """Mount a volume at a private mount point (nobrowse), or reuse where it already is; unmount what we mounted."""
    current = host.mount_point(identifier)
    if current:
        info = host.disk_info(identifier) or {}
        if not read_only and not info.get("WritableVolume"):
            raise host.HostError("{} is mounted read-only at {}".format(identifier, current))
        yield Path(current)
        return
    with tempfile.TemporaryDirectory(prefix="ioswitch-") as directory:
        args = ["mount"]
        if read_only:
            args.append("readOnly")
        args += ["-mountOptions", "nobrowse", "-mountPoint", directory, identifier]
        host.run("diskutil", *args, log=log)
        try:
            if owners and os.geteuid() == 0:
                host.run("diskutil", "enableOwnership", directory, log=log)
            yield Path(directory)
        finally:
            host.run("sync")
            host.run("diskutil", "unmount", identifier, log=log, check=False)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(HASH_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def boot_entry_files(package):
    """{relative path on the FAT: source} of the boot package: switchroot/ioswitch/* and the hekate ini."""
    package = Path(package)
    files = {}
    entry = package / BOOT_ENTRY
    if not entry.is_dir() or not (package / INI).is_file():
        raise host.HostError("{} is not a boot package (no {}/ or {})".format(package, BOOT_ENTRY, INI))
    for path in sorted(entry.iterdir()):
        if path.is_dir():
            raise host.HostError("unexpected directory {} in the boot package".format(path))
        files["{}/{}".format(BOOT_ENTRY, path.name)] = path
    files[INI] = package / INI
    return files


def sync_boot_entry(fat_root, package, log, dry_run=False):
    """Make switchroot/ioswitch/ and bootloader/ini/ioswitch.ini on the FAT equal the package; nothing else."""
    fat_root = Path(fat_root)
    wanted = boot_entry_files(package)
    changed, removed = [], []
    for relative, source in wanted.items():
        target = fat_root / relative
        if target.is_file() and target.stat().st_size == source.stat().st_size and \
                sha256_file(target) == sha256_file(source):
            continue
        changed.append(relative)
        if not dry_run:
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(".ioswitch-partial-" + target.name)
            shutil.copyfile(source, partial)
            os.replace(partial, target)
    entry = fat_root / BOOT_ENTRY
    if entry.is_dir():
        for path in sorted(entry.iterdir()):
            relative = "{}/{}".format(BOOT_ENTRY, path.name)
            if relative not in wanted and path.is_file() and not path.name.startswith("._"):
                removed.append(relative)
                if not dry_run:
                    path.unlink()
    for relative in list(wanted) + removed:          # AppleDouble files, by name only
        double = (fat_root / relative).with_name("._" + Path(relative).name)
        if double.exists() and not dry_run:
            double.unlink()
    if not dry_run:
        host.run("sync")
    log("boot entry{}: {} {}, {} stale {}, {} unchanged".format(
        " (dry run)" if dry_run else "", len(changed), "to update" if dry_run else "updated", len(removed),
        "to remove" if dry_run else "removed", len(wanted) - len(changed)), changed + ["-" + r for r in removed])
    return changed, removed


def tree_digest(root):
    root = Path(root)
    digest = hashlib.sha256()
    if not root.is_dir():
        return None
    for path in sorted(root.rglob("*")):
        if path.name.startswith("._"):
            continue
        relative = path.relative_to(root).as_posix()
        if path.is_dir():
            digest.update(b"d " + relative.encode() + b"\n")
        else:
            digest.update("f {} {:o} {}\n".format(relative, path.stat().st_mode & 0o777 | 0o200,
                                                    sha256_file(path)).encode())
    return digest.hexdigest()


def sync_overlay(data_root, package, log, dry_run=False):
    """<Data>/ioswitch/overlay := the package's overlay/ (whole tree, like the old deploy script)."""
    source, target = Path(package) / "overlay", Path(data_root) / "ioswitch/overlay"
    if not source.is_dir():
        log("overlay: the boot package has none; left as is")
        return False
    if tree_digest(source) == tree_digest(target):
        log("overlay: unchanged")
        return False
    if not dry_run:
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target, copy_function=shutil.copyfile)
        for path in [source, *source.rglob("*")]:
            copied = target / path.relative_to(source)
            mode = (path.stat().st_mode & 0o777) | 0o200      # Bazel outputs are read-only; keep the x bits
            copied.chmod(mode | (0o755 if path.is_dir() else 0))
            if os.geteuid() == 0:
                os.chown(copied, 0, 0)
        for leftover in target.rglob("._*"):
            leftover.unlink()
        host.run("sync")
    log("overlay: replaced ({} files)".format(sum(1 for p in source.rglob("*") if p.is_file())))
    return True
