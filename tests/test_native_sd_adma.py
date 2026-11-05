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
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
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

    def test_high_proof_guards(self):
        """r12-sdio: ADMA2 above 4 GiB is only used as far as it is proven, and a failed proof never reaches a client."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        header = (DRIVER / "IOSwitchAdma.h").read_text()
        # Every table takes its limit and page filter from the proof state, per direction.
        build = source.split("IOSwitchAdmaRefusal NativeIOSwitchBlock::buildTable(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("table.limit = high.limit(write);", build)
        self.assertIn("cacheableRam(page) && high.pageOk(page, write)", build)
        # Writes never go above 4 GiB while that is unproven; the IRAM alias window stays out of proving reads.
        self.assertIn("state == kOn || (state == kProving && !write) ? kAdmaDmaLimit : kAdma4GiB", header)
        self.assertIn("!(state == kProving && !write && page - kAdmaIramAlias < kAdmaIramBytes)", header)
        # Only reads are poisoned, and a mismatch puts SDMA's bytes into the requests before they complete.
        direct = source.split("int NativeIOSwitchBlock::directChain(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("poison && !write", direct)
        prove = source.split("bool NativeIOSwitchBlock::proveHigh(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("high.refuse();", prove)
        self.assertIn("stageChain(first, 0, length, checkBuffer, true)", prove)
        self.assertLess(prove.index("high.refuse();"), prove.index("stageChain(first, 0, length, checkBuffer, true)"))
        # A failed SDMA reference read proves nothing: the chain goes through the bounce buffer again.
        self.assertIn("return bounceChain(first, sectors);", prove)
        batch = source.split("void NativeIOSwitchBlock::serviceBatch(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("high.proofDue(write, table.high)", batch)
        self.assertIn("if (!proveHigh(first, sectors)) result = kIOReturnIOError;", batch)
        # A write from pages above 4 GiB is read back while those checks last.
        self.assertIn("high.writeCheckDue(write, table.high)", batch)


if __name__ == "__main__":
    unittest.main()
