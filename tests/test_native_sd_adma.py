import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "out/native-a57/root-v1/tree/drivers/NativeIOSwitchBlock"


class NativeSdAdmaTests(unittest.TestCase):
    def test_descriptor_tables(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sd-adma"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER), str(ROOT / "tests/native_sd_adma_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: sd adma2 tables", result.stdout)

    def test_driver_guards(self):
        """The driver keeps its partition guards, never polls ADMA2 and never treats its DMA interrupt as SDMA's."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        # Every card write is range-checked in command() itself, besides serviceBatch's check of the whole chain.
        command = source.split("int NativeIOSwitchBlock::command(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("outside the APFS partition", command)
        self.assertIn("outside the APFS partition", source.split("void NativeIOSwitchBlock::serviceBatch(", 1)[1])
        # In ADMA2 mode the SDMA boundary interrupt is neither awaited nor acted on (resuming would rewrite the
        # descriptor table's address), and an ADMA error is the host's failure.
        self.assertIn("INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS", command)
        self.assertIn("if (!descriptors && (s & INT_DMA))", command)
        self.assertLess(command.index("return kStepAdmaError"), command.index("resume += kSdmaBoundary"))
        # The data path selects SDMA or ADMA2 for every command, and the bring-up's SDMA read selects SDMA.
        self.assertIn("(r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)", command)
        card = source.split("int NativeIOSwitchBlock::cardCommand(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT)", card)
        # Writes stay at 256 KiB per command until boots have verified ADMA2 writes (proveWrite).
        self.assertRegex(source, r"constexpr uint32_t kDirectWriteSectors = kSafeSectors;")
        # Cache maintenance is by line over the table's own ranges: no set/way or whole-cache operation.
        self.assertIsNone(re.search(r"dc\s+(c?isw|csw)|FlushPoC_Dcache\b|CleanPoC_Dcache\b", source))


if __name__ == "__main__":
    unittest.main()
