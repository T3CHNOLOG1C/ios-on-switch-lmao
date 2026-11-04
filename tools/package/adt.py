"""adt.py SEED_HEX OUT: the native OLED board's ADT (native_oled_adt.generate_adt for the full-RAM root-boot profile),
which the CMake flow read from passes/native-root-memory-v1/full-ram.adt. Its only nondeterminism, the 64-byte
/chosen/random-seed, is pinned (the value that file carries), so the result is byte-identical to it."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_oled_adt  # noqa: E402

seed, out = sys.argv[1:]
Path(out).write_bytes(native_oled_adt.generate_adt(bytes.fromhex(seed), storage_probe=True, root_boot=True, full_ram=True))
