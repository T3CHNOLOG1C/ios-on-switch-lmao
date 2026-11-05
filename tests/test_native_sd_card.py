import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "out/native-a57/root-v1/tree/drivers/NativeIOSwitchBlock"


class NativeSdCardTests(unittest.TestCase):
    def test_register_decoders(self):
        """r14-sdcq: CID/CSD/SCR/SD Status, the extension registers and the command encodings, bit for bit."""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sd-card"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER), str(ROOT / "tests/native_sd_card_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: sd card registers", result.stdout)

    def test_identification_is_read_only(self):
        """identify() reads the card's registers once at boot and writes nothing: no CMD49, no data written."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        identify = body("void NativeIOSwitchBlock::identify()")
        for command in ("appCommand(51,", "appCommand(13,", "extRead(0, 0, kSector, regs)"):
            self.assertIn(command, identify)
        self.assertNotIn("cardCommand(49", identify)
        self.assertNotIn("w16(COMMAND", identify)
        ext = body("bool NativeIOSwitchBlock::extRead(")
        self.assertIn("cardCommand(48, ioswitchSdExtArgument(fno, address, length)", ext)
        self.assertIn("if (!ioswitchSdExtRange(address, length)) return false;", ext)
        # Once at boot, right after the bus is up (so the identification lines come first), never on resume.
        setup = body("bool NativeIOSwitchBlock::setup()")
        self.assertLess(setup.index("raiseBusSpeed()"), setup.index("identify();"))
        self.assertLess(setup.index("identify();"), setup.index("checkAdma("))
        self.assertNotIn("identify();", body("void NativeIOSwitchBlock::resume()"))
        reinit = body("bool NativeIOSwitchBlock::reinitialize(")
        self.assertIn("if (!resume && !cardCommand(9, rca << 16, RSP_R2, nullptr))", reinit)
        self.assertLess(reinit.index("cardCommand(9, rca << 16"), reinit.index("cardCommand(7, rca << 16"))


if __name__ == "__main__":
    unittest.main()
