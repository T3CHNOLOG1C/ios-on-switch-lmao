#!/usr/bin/env python3
"""Pin an IPSW's members for bazel/ipsw/pins.bzl, without storing the IPSW.

    bazel run //bazel/ipsw:pin -- <ipsw url> <product e.g. iPad7,1> <device class e.g. j120ap> > bazel/ipsw/pins.bzl

Reads the zip's central directory and BuildManifest.plist over HTTP range requests, selects the Erase build
identity for the device class, then streams each member the build needs once to compute two sha256s:
`sha256` of the member's bytes as stored in the zip (what Bazel downloads with a Range header and keeps in its
repository cache) and `content_sha256` of the member itself (after inflating a deflated member). Nothing is
written to disk. The OS dmg alone is several GB, so expect a few minutes.
"""
import hashlib
import json
import plistlib
import struct
import sys
import urllib.request
import zlib

CHUNK = 8 << 20
# BuildManifest components the boot and the SD install read, by their role in this build.
COMPONENTS = {
    "os": "OS",
    "system_cryptex": "Cryptex1,SystemOS",
    "app_cryptex": "Cryptex1,AppOS",
    "static_trust_cache": "StaticTrustCache",
    "system_cryptex_trust_cache": "Cryptex1,SystemTrustCache",
    "app_cryptex_trust_cache": "Cryptex1,AppTrustCache",
    "kernelcache": "KernelCache",
    "device_tree": "DeviceTree",
}


class Remote:
    def __init__(self, url):
        self.url = url
        request = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(request) as response:
            self.size = int(response.headers["Content-Length"])

    def range(self, start, end):
        request = urllib.request.Request(self.url, headers={"Range": "bytes={}-{}".format(start, end - 1)})
        response = urllib.request.urlopen(request)
        if response.status != 206:
            raise SystemExit("{}: no range support (HTTP {})".format(self.url, response.status))
        return response

    def read(self, start, end):
        with self.range(start, end) as response:
            return response.read()


def central_directory(remote):
    tail_start = max(0, remote.size - 65536 - 22)
    tail = remote.read(tail_start, remote.size)
    eocd = tail.rfind(b"PK\x05\x06")
    count, cd_size, cd_offset = struct.unpack_from("<HII", tail, eocd + 10)
    zip64 = tail.rfind(b"PK\x06\x06")
    if zip64 >= 0:
        count, cd_size, cd_offset = struct.unpack_from("<QQQ", tail, zip64 + 32)
    data, at, members = remote.read(cd_offset, cd_offset + cd_size), 0, {}
    while data[at:at + 4] == b"PK\x01\x02":
        method, crc, csize, usize, name_len, extra_len, comment_len = struct.unpack_from("<H4xIIIHHH", data, at + 10)
        header = struct.unpack_from("<I", data, at + 42)[0]
        name = data[at + 46:at + 46 + name_len].decode()
        extra, e = data[at + 46 + name_len:at + 46 + name_len + extra_len], 0
        while e + 4 <= len(extra):
            tag, length = struct.unpack_from("<HH", extra, e)
            if tag == 1:
                values = list(struct.unpack_from("<" + "Q" * (length // 8), extra, e + 4))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if header == 0xFFFFFFFF:
                    header = values.pop(0)
            e += 4 + length
        members[name] = {"method": method, "crc32": crc, "csize": csize, "size": usize, "header": header}
        at += 46 + name_len + extra_len + comment_len
    return members


def data_offset(remote, member):
    local = remote.read(member["header"], member["header"] + 30)
    if local[:4] != b"PK\x03\x04":
        raise SystemExit("bad local header")
    name_len, extra_len = struct.unpack_from("<HH", local, 26)
    return member["header"] + 30 + name_len + extra_len


def stream(remote, member, offset):
    """sha256 of the stored bytes and of the content; checks the zip's CRC and size."""
    stored, content = hashlib.sha256(), hashlib.sha256()
    inflate = zlib.decompressobj(-15) if member["method"] == 8 else None
    crc, size = 0, 0
    with remote.range(offset, offset + member["csize"]) as response:
        while chunk := response.read(CHUNK):
            stored.update(chunk)
            out = inflate.decompress(chunk) if inflate else chunk
            content.update(out)
            crc, size = zlib.crc32(out, crc), size + len(out)
    if inflate:
        tail = inflate.flush()
        content.update(tail)
        crc, size = zlib.crc32(tail, crc), size + len(tail)
    if (crc, size) != (member["crc32"], member["size"]):
        raise SystemExit("CRC/size mismatch")
    return stored.hexdigest(), content.hexdigest()


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    url, product, device_class = sys.argv[1:]
    remote = Remote(url)
    members = central_directory(remote)
    manifest_member = members["BuildManifest.plist"]
    raw = remote.read(data_offset(remote, manifest_member), data_offset(remote, manifest_member) + manifest_member["csize"])
    manifest = plistlib.loads(zlib.decompress(raw, -15) if manifest_member["method"] == 8 else raw)
    if product not in manifest["SupportedProductTypes"]:
        raise SystemExit("{} is not in this IPSW ({})".format(product, manifest["SupportedProductTypes"]))
    identity = next(i for i in manifest["BuildIdentities"]
                    if i["Info"]["DeviceClass"] == device_class and i["Info"]["RestoreBehavior"] == "Erase")
    pins = {}
    for role, component in sorted(COMPONENTS.items()):
        entry = identity["Manifest"].get(component)
        if not entry or "Path" not in entry.get("Info", {}):
            print("# {} ({}): not in this build identity".format(role, component), file=sys.stderr)
            continue
        name = entry["Info"]["Path"]
        member = members[name]
        offset = data_offset(remote, member)
        print("# {}: {} ({} bytes stored, {} bytes)".format(role, name, member["csize"], member["size"]), file=sys.stderr)
        sha256, content_sha256 = stream(remote, member, offset)
        pins[role] = {
            "member": name,
            "component": component,
            "offset": offset,
            "stored_size": member["csize"],
            "size": member["size"],
            "deflated": member["method"] == 8,
            "sha256": sha256,
            "content_sha256": content_sha256,
        }
    print('"""IPSW member pins, written by `bazel run //bazel/ipsw:pin` (bazel/ipsw/pin.py). Do not edit by hand."""\n')
    print("IPSW = " + json.dumps({
        "url": url,
        "size": remote.size,
        "product": product,
        "device_class": device_class,
        "version": manifest["ProductVersion"],
        "build": manifest["ProductBuildVersion"],
        "members": pins,
    }, indent = 4, sort_keys = True).replace(": true", ": True").replace(": false", ": False"))


if __name__ == "__main__":
    main()
