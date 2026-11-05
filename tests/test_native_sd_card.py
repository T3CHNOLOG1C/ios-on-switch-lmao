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

    def test_cmd23_framing_is_proven_and_gated(self):
        """Auto CMD23 only for multi-block ADMA2 commands, after setup()'s A/B reads match and the SCR lists CMD23; retries
        and every SDMA command (bounce path, proofs, setup reads) stop by CMD12 as before; CMD23 writes are read back."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        issue = body("int NativeIOSwitchBlock::issue(")
        self.assertIn("ioswitchSdBlockFrame(sectors, write, cmd23)", issue)
        # Argument 2 before the command that uses it, and the partition guard still first.
        self.assertLess(issue.index("outside the APFS partition"), issue.index("w32(ARGUMENT2, frame.argument2)"))
        self.assertLess(issue.index("w32(ARGUMENT2, frame.argument2)"), issue.index("w16(COMMAND, frame.command)"))
        complete = body("int NativeIOSwitchBlock::complete(")
        self.assertIn("if (r16(AUTO_CMD_ERROR) & 0x9f) return 8;", complete)
        self.assertIn("if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS)) return 9;", complete)
        # The data path: direct (ADMA2) multi-block commands only, per direction.
        begin = body("void NativeIOSwitchBlock::beginBatch(")
        self.assertIn("b.cmd23 = b.direct && sectors > 1 && (b.write ? cmd23Writes : cmd23Reads);", begin)
        # Retries go back to CMD12 (command()'s default), as do transfer()/run() (the bounce path and the proofs).
        self.assertIn("step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);",
                      body("int NativeIOSwitchBlock::retry("))
        self.assertIn("command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);",
                      body("int NativeIOSwitchBlock::run("))
        self.assertRegex(source, r"int command\(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, "
                                 r"uint32_t \*status, bool cmd23 = false\);")
        # setup(): gated on the SCR and on the A/B check; writes only with their read-backs.
        setup = body("bool NativeIOSwitchBlock::setup()")
        self.assertIn("if (!haveScr || !(scr.cmdSupport & kSdCmd23))", setup)
        self.assertLess(setup.index("checkCmd23(&withNs, &withoutNs)"), setup.index("cmd23Reads = true;"))
        self.assertIn("cmd23Writes = admaWrites;", setup)
        check = body("const char *NativeIOSwitchBlock::checkCmd23(")
        self.assertIn("memcmp(bounce, bounce + kBytes, kBytes)", check)
        self.assertIn("memset(bounce + at, with ? 0x00 : 0xff, kBytes);", check)
        end = body("void NativeIOSwitchBlock::endBatch(")
        self.assertIn("(b.cmd23 && cmd23WriteChecks)", end)
        self.assertIn("!proveWrite(first, sectors, b.cmd23)", end)
        prove = body("bool NativeIOSwitchBlock::proveWrite(")
        self.assertRegex(prove, r"if \(cmd23\) \{[^}]*cmd23Writes = false;")
        self.assertIn("(admaWrites && cmd23Writes && cmd23WriteChecks)", body("void NativeIOSwitchBlock::releaseChecks()"))


if __name__ == "__main__":
    unittest.main()
