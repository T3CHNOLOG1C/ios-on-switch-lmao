import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/NativeIOSwitchBlock"


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
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        # Every card write is range-checked where every data command is issued (issue(), which command() and the
        # worker's ADMA2 commands both go through), besides beginBatch's check of the whole chain.
        issue, complete = body("int NativeIOSwitchBlock::issue("), body("int NativeIOSwitchBlock::complete(")
        self.assertIn("outside the APFS partition", issue)
        self.assertLess(issue.index("outside the APFS partition"), issue.index("w16(COMMAND"))
        self.assertIn("issue(lba, sectors, write, descriptors, cmd23)", body("int NativeIOSwitchBlock::command("))
        begin = body("void NativeIOSwitchBlock::beginBatch(")
        self.assertIn("outside the APFS partition", begin)
        self.assertLess(begin.index("outside the APFS partition"), begin.index("issue("))
        # issue() is only called from command() and beginBatch(), and (r14-sdcq) for command-queue reads from
        # beginTask() and checkCq(): those issue reads (write false) with a task argument, never a write.
        calls = re.findall(r"^.*[^:\w]issue\(static_cast|^.*[^:\w]issue\(lba", source, re.M)
        self.assertEqual(len(calls), 4, calls)
        for name in ("void NativeIOSwitchBlock::beginTask(", "const char *NativeIOSwitchBlock::checkCq("):
            self.assertRegex(body(name), r"issue\(static_cast<uint32_t>\([^)]*\), \w+(?:\.sectors)?, false, [^;]*cq\.argument\(\)\)")
        # In ADMA2 mode the SDMA boundary interrupt is neither awaited nor acted on (resuming would rewrite the
        # descriptor table's address), and an ADMA error is the host's failure.
        self.assertIn("INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS", complete)
        self.assertIn("if (!descriptors && (s & INT_DMA))", complete)
        self.assertLess(complete.index("return kStepAdmaError"), complete.index("resume += kSdmaBoundary"))
        # The data path selects SDMA or ADMA2 for every command, and the bring-up's SDMA read selects SDMA.
        self.assertIn("(r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)", issue)
        card = source.split("int NativeIOSwitchBlock::cardCommand(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT)", card)
        # r12-sdio: ADMA2 writes up to 1 MiB, each boot's first ones over 256 KiB read back (proveWrite); one that
        # differs takes writes over 256 KiB back to the bounce path's 256 KiB SDMA commands (never SDMA over 256 KiB).
        self.assertRegex(source, r"constexpr uint32_t kDirectWriteSectors = kBounceSectors;")
        self.assertRegex(source, r"constexpr uint32_t kWriteChecks = 32, kLargeWriteChecks = 16;")
        prove = body("bool NativeIOSwitchBlock::proveWrite(")
        self.assertIn("if (sectors > kSafeSectors && largeWriteChecks) --largeWriteChecks;", prove)
        self.assertRegex(prove, r"if \(sectors > kSafeSectors\) \{[^}]*writeSectors = kSafeSectors;")
        self.assertIn("admaWrites = false;", prove)
        self.assertIn("(admaWrites && sectors <= writeSectors)", body("void NativeIOSwitchBlock::beginBatch("))
        self.assertIn("(sectors > kSafeSectors && largeWriteChecks)", body("void NativeIOSwitchBlock::endBatch("))
        self.assertIn("const uint32_t limit = first->write ? writeSectors : maxSectors;",
                      body("Request *NativeIOSwitchBlock::pickLocked("))
        self.assertIn("const uint32_t limit = write ? kSafeSectors : sdmaReadSectors;",
                      body("bool NativeIOSwitchBlock::bounceChain("))
        self.assertIn("largeWriteChecks = checkBuffer ? kLargeWriteChecks : 0;", source)
        self.assertIn("writeSectors = admaWrites ? kDirectWriteSectors : kSafeSectors;", source)
        # Cache maintenance is by line over the table's own ranges: no set/way or whole-cache operation.
        self.assertIsNone(re.search(r"dc\s+(c?isw|csw)|FlushPoC_Dcache\b|CleanPoC_Dcache\b", source))

    def test_high_proof_guards(self):
        """r12-sdio: ADMA2 above 4 GiB is only used as far as it is proven, and a failed proof never reaches a client."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        header = (DRIVER / "IOSwitchAdma.h").read_text()
        # Every table takes its limit and page filter from the proof state, per direction.
        build = source.split("IOSwitchAdmaRefusal NativeIOSwitchBlock::buildTable(", 1)[1].split("\n}\n", 1)[0]
        # r14-sdcq: a command-queue task stays below 4 GiB while that is unproven (its proof is a legacy command).
        self.assertIn("const uint64_t limit = task && high.state == IOSwitchAdmaHigh::kProving ? kAdma4GiB : high.limit(write);",
                      build)
        self.assertEqual(build.count("table.limit = limit;"), 2)
        self.assertIn("cacheableRam(page) && high.pageOk(page, write)", build)
        # Writes never go above 4 GiB while that is unproven; the IRAM alias window stays out of proving reads.
        self.assertIn("state == kOn || (state == kProving && !write) ? kAdmaDmaLimit : kAdma4GiB", header)
        self.assertIn("!(state == kProving && !write && page - kAdmaIramAlias < kAdmaIramBytes)", header)
        # Only reads are poisoned, and a mismatch puts SDMA's bytes into the requests before they complete.
        direct = source.split("void NativeIOSwitchBlock::prepareDirect(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("poison && !write", direct)
        prove = source.split("bool NativeIOSwitchBlock::proveHigh(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("high.refuse();", prove)
        self.assertIn("stageChain(first, 0, length, checkBuffer, true)", prove)
        self.assertLess(prove.index("high.refuse();"), prove.index("stageChain(first, 0, length, checkBuffer, true)"))
        # A failed SDMA reference read proves nothing: the chain goes through the bounce buffer again.
        self.assertIn("return bounceChain(first, sectors);", prove)
        self.assertIn("high.proofDue(b.write, table.high)", source.split("void NativeIOSwitchBlock::beginBatch(", 1)[1])
        batch = source.split("void NativeIOSwitchBlock::endBatch(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("if (!proveHigh(first, sectors)) result = kIOReturnIOError;", batch)
        # A write from pages above 4 GiB is read back while those checks last.
        self.assertIn("high.writeCheckDue(write, table.high)", batch)


    def test_pipelined_worker(self):
        """r12-sdio: the next chain's command is issued before the previous chain completes; waits stay on the interrupt."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        worker = source.split("void NativeIOSwitchBlock::worker(", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(worker.index("beginBatch(*next)"), worker.index("completeBatch(*pending)"))
        self.assertLess(worker.index("completeBatch(*pending)"), worker.index("endBatch(*next)"))
        # suspend() waits for `working`: it is set before the command is begun and cleared only after endBatch (r14-sdcq:
        # and around leaving CQ mode when the queue runs dry, before the pick).
        self.assertLess(worker.index("working = true", worker.index("pickLocked(")), worker.index("beginBatch(*next)"))
        self.assertLess(worker.index("endBatch(*next)"), worker.rindex("working = false"))
        self.assertLess(worker.index("runGroup("), worker.rindex("working = false"))
        # beginBatch never waits for the card: it only issues (no complete(), command(), run() or transfer() there).
        begin = source.split("void NativeIOSwitchBlock::beginBatch(", 1)[1].split("\n}\n", 1)[0]
        self.assertIsNone(re.search(r"\b(complete|command|run|transfer|bounceChain|waitSignal)\(", begin))
        # complete(): STATUS first (a command that finished meanwhile takes no interrupt), then the interrupt; its time
        # limit starts there; a lost interrupt still panics, and polling only without the interrupt (ioswitch-sd-irq=0).
        complete = source.split("int NativeIOSwitchBlock::complete(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("const uint64_t start = nowNs();", complete)
        self.assertLess(complete.index("s = r32(STATUS);"), complete.index("waitSignal("))
        self.assertIn("SDMMC1 interrupt lost", complete)
        self.assertLess(complete.index("if (irqMode)"), complete.index("IOSleep(1)"))
        # endBatch: a split command's errors take run()'s retry, and a read's pages are invalidated after the command.
        end = source.split("void NativeIOSwitchBlock::endBatch(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("retry(step, status, first->lba, sectors, write, table.count)", end)
        self.assertLess(end.index("retry("), end.index("finishDirect(write)"))
        self.assertLess(end.index("finishDirect(write)"), end.index("proveHigh("))


    def test_read_staging(self):
        """r12-sdio: a read's refused segments alone go through the bounce buffer, once setup() has run that path."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        build = body("IOSwitchAdmaRefusal NativeIOSwitchBlock::buildTable(")
        # Reads only, and only once the self-check has passed; writes keep the whole-chain bounce.
        self.assertIn("if (!write && stagingOn) {", build)
        # The staged segments are copied out after the read's invalidate and before any proof reuses the buffer.
        end = body("void NativeIOSwitchBlock::endBatch(")
        self.assertLess(end.index("finishDirect(write)"), end.index("copyStaged()"))
        self.assertLess(end.index("copyStaged()"), end.index("proveHigh("))
        # proveHigh compares the requests' own pages only, never the bounce buffer's runs.
        self.assertIn("pa >= bouncePA && pa - bouncePA < kBounceBytes", body("bool NativeIOSwitchBlock::proveHigh("))
        # setup()'s check runs the staging path on the hardware; a failure only keeps it off (no panic in it).
        check = body("const char *NativeIOSwitchBlock::checkAdma(")
        self.assertIn("stagingOn = !stagingWhy;", check)
        self.assertLess(check.index("copyStaged()"), check.index("memcmp(pageBytes, reference, kTestBytes)"))
        self.assertNotIn("panic(", check)


if __name__ == "__main__":
    unittest.main()
