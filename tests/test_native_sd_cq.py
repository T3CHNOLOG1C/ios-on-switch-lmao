import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "out/native-a57/root-v1/tree/drivers/NativeIOSwitchBlock"


class NativeSdCommandQueueTests(unittest.TestCase):
    def test_engine_against_a_model_card(self):
        """r14-sdcq: the command-queue engine never sends a command the card's rules forbid, and always gets the card out
        of CQ mode (or says it cannot) when anything fails."""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sd-cq"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER), str(ROOT / "tests/native_sd_cq_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: sd command queue", result.stdout)

    def test_driver_guards(self):
        """What the driver's command-queue path may and may not do."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        # The only extension-register write: the CQ control byte, 00h or 03h, through CqBus::control, which refuses
        # (panics on) anything else before the command is issued.
        control = body("int NativeIOSwitchBlock::CqBus::control(")
        self.assertIn("address != ioswitchSdCqControlAddress(self->extInfo.perf)", control)
        self.assertIn("*value != 0 && *value != (kSdCqEnable | kSdCqSequential)", control)
        self.assertLess(control.index("panic("), control.index("cqCommand(write ? 49 : 48"))
        self.assertIn("ioswitchSdExtArgument(self->extInfo.perf.fno, address, 1)", control)
        self.assertEqual(len(re.findall(r"cqCommand\(write \? 49|cqCommand\(49|cardCommand\(49", source)), 1)
        # Interrupts, never polling: cqCommand waits by waitSignal (a lost interrupt panics); the only timed wait in the
        # queue's code is the engine's settle() between read-backs of a card slow to leave CQ mode.
        command = body("int NativeIOSwitchBlock::cqCommand(")
        self.assertIn("waitSignal(awaited,", command)
        self.assertIn('panic("IOSwitchSD: SDMMC1 interrupt lost', command)
        self.assertNotIn("IOSleep", command)
        self.assertNotIn("IODelay", command)
        self.assertNotIn("13, ", body("Batch *NativeIOSwitchBlock::runGroup("))  # no CMD13 task-status polling
        engine = (DRIVER / "IOSwitchCq.h").read_text()
        self.assertEqual(engine.count("bus.settle()"), 1)
        self.assertIn("kSdCqEnable | kSdCqSequential", engine)
        # Reads only: a task write panics before anything is issued.
        issue = body("int NativeIOSwitchBlock::issue(")
        self.assertIn("if (write && task != kNoTask) panic(", issue)
        self.assertLess(issue.index("if (write && task != kNoTask) panic("), issue.index("w16(COMMAND"))
        group = body("unsigned NativeIOSwitchBlock::groupLocked(")
        self.assertIn("first->write || sectors > kCqTaskSectors", group)
        self.assertIn("ioswitchNextIndependentRead(head, kCqTaskSectors, kMergeScan)", group)
        self.assertIn("if (payload != sectors && gapProofs) return 0;", group)
        # Every task's blocks inside the APFS partition, checked before the group is assigned.
        run = body("Batch *NativeIOSwitchBlock::runGroup(")
        self.assertLess(run.index("outside the APFS partition"), run.index("cq.submit(cqBus, tasks, n)"))
        # The first tasks of every boot are held and compared with legacy reads (out of CQ mode) before completing.
        self.assertIn("const bool proving = cqProofs != 0;", run)
        self.assertLess(run.index("if (!cq.leave(cqBus))"), run.index("compareChain(group[i].first"))
        self.assertIn("if (!proving) pending = &b;", run)
        # A proof that differs stops the queue and the whole group is read again the legacy way (from task 0).
        self.assertRegex(run, r"if \(proof == kChainDiffers\) \{[^}]*why = cqWhy;")
        self.assertIn("for (unsigned i = proving ? 0 : done; i < n; ++i) {", run)
        self.assertIn("cqProofs = kCqProofs;", body("bool NativeIOSwitchBlock::setup()"))
        # A failure stops the queue first, then the chains not completed run the legacy way.
        self.assertLess(run.index("cqStop(why);"), run.index("beginBatch(b);"))
        # A legacy command never finds the card in CQ mode: the worker leaves it first, and when its queue runs dry.
        worker = body("void NativeIOSwitchBlock::worker(")
        self.assertLess(worker.index("if (next) self->cqLeave();"), worker.index("if (next) self->beginBatch(*next);"))
        self.assertIn("self->cq.state == CqEngine::kOn) {", worker)
        self.assertIn("if (cq.state == CqEngine::kOn) (void)cq.leave(cqBus);", body("void NativeIOSwitchBlock::suspend()"))
        self.assertIn("cq.powerCycled();", body("void NativeIOSwitchBlock::resume()"))
        # A card that will not leave CQ mode is power-cycled the way system sleep does it, never polled forever.
        stop = body("void NativeIOSwitchBlock::cqStop(")
        self.assertIn("resetCard();", stop)
        reset = body("void NativeIOSwitchBlock::resetCard()")
        for step in ("powerOffCard();", "powerOnController();", "reinitialize(", "memcmp(again, gptHeader"):
            self.assertIn(step, reset)
        # Setup: only a card that offers a queue with its control byte off, and only after its test group matched.
        check = body("const char *NativeIOSwitchBlock::checkCq(")
        self.assertIn('if (perf.cqDepth < 2) return "the card offers no queue";', check)
        self.assertIn("if (perf.cqControl & kSdCqEnable)", check)
        self.assertIn("memcmp(bounce + i * kBytes, checkBuffer + i * kBytes, kBytes)", check)
        self.assertLess(check.index("cq.start(perf.cqDepth);"), check.index("cq.enter(cqBus)"))


if __name__ == "__main__":
    unittest.main()
