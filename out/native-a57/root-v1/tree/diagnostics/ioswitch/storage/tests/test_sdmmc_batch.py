"""Run only the new multi-block transport's register model under sanitizers."""
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
STORAGE = HERE.parent

def main():
    compiler = shutil.which("clang")
    if compiler is None:
        raise SystemExit("clang is required for the focused transport check")
    with tempfile.TemporaryDirectory(prefix="iosw-sd-batch-") as directory:
        binary = Path(directory) / "sd-batch-test"
        subprocess.run([
            compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(STORAGE), str(HERE / "sdmmc_batch_test.c"),
            str(STORAGE / "sdmmc_read.c"), str(STORAGE / "sdmmc_dma.c"), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

if __name__ == "__main__":
    main()
