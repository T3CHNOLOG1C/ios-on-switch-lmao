"""Every pinned file matches SHA256SUMS (and nothing pinned is missing from it)."""
import hashlib
import lzma
import sys
from pathlib import Path

sums = Path(sys.argv[1])
root = sums.parent
bad = []
for line in sums.read_text().splitlines():
    digest, name = line.split("  ", 1)
    if name.endswith(" (decompressed)"):
        data = lzma.decompress((root / (name[:-len(" (decompressed)")] + ".xz")).read_bytes())
    else:
        data = (root / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        bad.append(name)
listed = {line.split("  ", 1)[1].replace(" (decompressed)", ".xz") for line in sums.read_text().splitlines()}
# Every file in the pinned directories (the runfiles tree also holds this test's own interpreter files).
dirs = {name.split("/")[0] for name in listed}
present = {str(p.relative_to(root)) for d in dirs for p in (root / d).rglob("*") if p.is_file()}
unlisted = sorted(present - listed)
if bad or unlisted:
    sys.exit("sha256 mismatch: {}; not in SHA256SUMS: {}".format(bad, unlisted))
print("{} pinned files verified".format(len(listed)))
