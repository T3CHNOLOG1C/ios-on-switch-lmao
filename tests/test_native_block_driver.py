import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeBlockDriverTests(unittest.TestCase):
    def test_actual_driver_with_host_iokit_and_backend_mocks(self):
        """Host behavior only: the real driver compiles separately against XNU."""
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            headers = [
                "IOKit/IOBSD.h", "IOKit/IOLib.h", "IOKit/IOKitKeys.h",
                "IOKit/storage/IOBlockStorageDevice.h", "libkern/c++/OSDictionary.h",
                "libkern/c++/OSIterator.h", "libkern/c++/OSNumber.h", "libkern/c++/OSString.h",
            ]
            for name in headers:
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "' + str(ROOT / "tests/native_block_stubs.h") + '"\n')
            binary = temp / "native-block-test"
            compile_result = subprocess.run([
                "xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-fsanitize=address,undefined", "-g", "-pthread",
                "-I", str(temp), str(ROOT / "tests/native_block_harness.cpp"), "-o", str(binary),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: block driver", result.stdout)


if __name__ == "__main__":
    unittest.main()
