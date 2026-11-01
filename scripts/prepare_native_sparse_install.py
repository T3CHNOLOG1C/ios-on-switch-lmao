#!/usr/bin/env python3
"""Plan retained FAT prefixes and encode IWI2 from a proved APFS copy map.

Only new host output files are written. No SD path or image contents are opened.
"""
from pathlib import Path
import argparse
import hashlib
import json
import stat
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.make_native_install_manifest import fat_path

REQUIRED_UNION = {'all_host_data_extents', 'all_main_allocated_bits',
    'whole_checkpoint_descriptor_area', 'whole_checkpoint_data_area',
    'whole_internal_pool', 'whole_ip_bitmap_ring', 'block_zero'}


def regular(path):
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)) or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('expected nonsymlink regular file: ' + str(path))
    return path


def identity(s):
    return dict(device=s.st_dev, inode=s.st_ino, size=s.st_size, mtime_ns=s.st_mtime_ns,
                ctime_ns=s.st_ctime_ns, allocated_bytes=s.st_blocks*512, uid=s.st_uid)


def plan(parts, logical, block, ranges):
    if (not 1 <= len(parts) <= 128 or block < 4096 or block > 65536 or block & (block-1)
            or logical <= 0 or logical % block or logical // 512 > 0xffffffff):
        raise ValueError('invalid logical geometry or part count')
    bases = []; total = 0; names = set()
    for name, size in parts:
        fat_path(name)
        if name.casefold() in names or size <= 0 or size % block or size > 0xfffffe00:
            raise ValueError('invalid original part path or geometry')
        names.add(name.casefold()); bases.append(total); total += size
    if total != logical or not ranges or len(ranges) > 8192:
        raise ValueError('original parts must cover the logical image and have bounded copy ranges')
    high = [0] * len(parts); low = [None] * len(parts); pieces = []; previous = 0
    for index, (start, end) in enumerate(ranges):
        if (not isinstance(start, int) or not isinstance(end, int) or start < previous
                or start >= end or end > logical or (start | end) % block
                or (index == 0 and (start != 0 or end < block))):
            raise ValueError('copy ranges must be ordered disjoint APFS blocks, starting at block zero')
        previous = end; at = start
        for i, ((name, size), base) in enumerate(zip(parts, bases)):
            if at >= end: break
            if at >= base + size: continue
            if at < base: raise ValueError('copy range lost original source coverage')
            stop = min(end, base + size)
            high[i] = max(high[i], stop-base)
            low[i] = at-base if low[i] is None else min(low[i], at-base)
            pieces.append((at, i, at-base, stop-at)); at = stop
        if at != end: raise ValueError('copy range extends beyond source parts')
    retained = []; actions = []; source_bases = {}; source_bytes = 0; fresh = []; fresh_bytes = 0
    for i, ((name, size), base, keep) in enumerate(zip(parts, bases, high)):
        origin = 0; replacement = None
        if (keep and low[i] >= 1 << 20 and keep-low[i] <= 1 << 20
                and fresh_bytes+keep-low[i] <= 1 << 20):
            origin = low[i]; replacement = 'IOSWITCH/INSTALL/RAW27/' + (
                'TAIL.BIN' if i == len(parts)-1 else f'SLICE{i:04d}.BIN')
            fresh.append(dict(path=replacement, image_offset=base+origin, bytes=keep-origin))
            fresh_bytes += keep-origin
        action = 'delete' if not keep or replacement else 'keep' if keep == size else 'truncate'
        actions.append(dict(path=name, original_image_offset=base, old_bytes=size,
                            retained_bytes=0 if replacement else keep, action=action,
                            replacement_path=replacement))
        if keep:
            source_bases[i] = (source_bytes, origin)
            retained.append((replacement or name, keep-origin)); source_bytes += keep-origin
    extents = []
    for target, i, within, size in pieces:
        base, origin = source_bases[i]; source = base + within-origin
        if extents and extents[-1][0]+extents[-1][2] == target and extents[-1][1]+extents[-1][2] == source:
            extents[-1][2] += size
        else: extents.append([target, source, size])
    copied = sum(row[2] for row in extents)
    return dict(logical_bytes=logical, block_size=block, source_bytes=source_bytes,
                copied_bytes=copied, skipped_target_bytes=logical-copied,
                released_FAT_bytes=logical-source_bytes, parts=retained,
                extents=extents, actions=actions, new_files=fresh,
                new_small_copy_bytes=fresh_bytes)


def encode(result):
    parts = result['parts']; extents = result['extents']; logical = result['logical_bytes']
    block = result['block_size']; source_bytes = result['source_bytes']
    if not 1 <= len(parts) <= 128 or not 1 <= len(extents) <= 8192:
        raise ValueError('IWI2 part or extent limit exceeded')
    if (block < 4096 or block > 65536 or block & (block-1) or logical <= 0 or logical % block
            or logical // 512 > 0xffffffff or sum(size for _, size in parts) != source_bytes
            or not 0 < source_bytes <= logical):
        raise ValueError('IWI2 source byte count differs')
    records = []; names = set()
    for name, size in parts:
        if size <= 0 or size % 512 or size > 0xfffffe00 or name.casefold() in names:
            raise ValueError('invalid IWI2 part')
        names.add(name.casefold())
        records.append(fat_path(name).ljust(240, b'\0') + struct.pack('<Q', size) + bytes(8))
    previous_image = previous_source = 0
    for index, (image, source, length) in enumerate(extents):
        if (min(image, source) < 0 or length <= 0 or (image | source | length) % block
                or image < previous_image or source < previous_source
                or length > logical-image or length > source_bytes-source
                or (index == 0 and (image or source or length < block))):
            raise ValueError('invalid IWI2 extent geometry')
        previous_image = image+length; previous_source = source+length
    return (struct.pack('<8sIIQIIQ24s', b'IOSWIMG2', 2, len(parts), logical, block,
                        len(extents), source_bytes, bytes(24)) + b''.join(records)
            + b''.join(struct.pack('<QQQ', *row) for row in extents))


def prepare(copy_map, candidate, stage, output):
    map_path = regular(copy_map); map_bytes = map_path.read_bytes(); proof = json.loads(map_bytes)
    prepared = json.loads(regular(candidate).read_text()); staged = json.loads(regular(stage).read_text())
    image = regular(prepared['image'])
    if (proof['status'] != 'pass' or proof['complete'] is not True
            or proof['content_scan'] is not False or proof['image_writes'] != 0
            or set(proof['required_union']) != REQUIRED_UNION
            or proof['candidate_stat'] != prepared['candidate_stat']
            or identity(image.stat()) != proof['candidate_stat']
            or prepared['state'] != 'full_data_and_cryptex_staged_detached'
            or prepared['final_candidate_detached'] is not True
            or proof['logical_bytes'] != prepared['logical_bytes']
            or staged['image'] != str(image) or staged['image_bytes'] != proof['logical_bytes']
            or staged['state'] != 'complete' or staged['all_new_files_verified'] is not True
            or staged['entry_published'] is not True or staged['android_writes_by_host'] is not False):
        raise ValueError('complete matching APFS allocation proof and unchanged staged candidate required')
    expected = [f'IOSWITCH/INSTALL/RAW24/APFS{i:04d}.BIN' for i in range(16)]
    rows = [row for row in staged['files'] if row['path'] in set(expected)]
    by_name = {row['path']: row for row in rows}
    if len(rows) != 16 or set(by_name) != set(expected) or any(by_name[name]['size_bytes'] != 2 << 30 for name in expected):
        raise ValueError('exact original sixteen 2GiB staged parts required')
    result = plan([(name, by_name[name]['size_bytes']) for name in expected],
                  proof['logical_bytes'], proof['block_size'], proof['copy_extents'])
    if result['copied_bytes'] != proof['copy_bytes'] or result['skipped_target_bytes'] != proof['skipped_bytes']:
        raise ValueError('copy map accounting differs')
    data = encode(result); output = Path(output)
    if (not output.resolve().is_relative_to(ROOT / 'out') or output.exists()
            or any(p.is_symlink() for p in (output, *output.parents))):
        raise ValueError('new nonsymlink output directory below workspace out required')
    output.mkdir()
    with (output / 'RAW27.IWI').open('xb') as stream: stream.write(data)
    if (output / 'RAW27.IWI').read_bytes() != data: raise ValueError('new manifest readback differs')
    result.update(status='plan_only', manifest='RAW27.IWI', manifest_bytes=len(data),
        manifest_sha256=hashlib.sha256(data).hexdigest(), copy_map=str(map_path),
        copy_map_sha256=hashlib.sha256(map_bytes).hexdigest(), candidate_stat=proof['candidate_stat'],
        staged_image_sha256=staged['image_sha256'], allocation_proof=proof,
        image_contents_read=False, device_access=False, SD_mutations_performed=False,
        target_holes='Proved APFS-free ranges omitted; existing target bytes remain untouched.')
    (output / 'plan.json').write_text(json.dumps(result, indent=2) + '\n')
    return {key: result[key] for key in ('status', 'logical_bytes', 'source_bytes', 'copied_bytes',
        'skipped_target_bytes', 'released_FAT_bytes', 'manifest_bytes', 'manifest_sha256')}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--copy-map', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, default=ROOT / 'out/native-single-apfs-trial-v1/candidate.json')
    parser.add_argument('--stage', type=Path, default=ROOT / 'out/switch/native-apfs-installer-v24-stage.json')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.copy_map, args.candidate, args.stage, args.output_dir), indent=2))
