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
        # The only extension-register writes: r16-sdcq: Cache Enable 00h/01h, Flush Cache 01h and the CQ control byte
        # 00h/03h, through CqBus::control, which refuses (panics on) anything else before the command is issued.
        control = body("int NativeIOSwitchBlock::CqBus::control(")
        self.assertIn("cqAt != ioswitchSdCqControlAddress(self->extInfo.perf)", control)
        self.assertIn("write ? ioswitchSdPerfWriteAddress(self->extInfo.perf, byte, *value)", control)
        self.assertIn("address != cqAt - (kSdPerfCqControl - byte)", control)
        self.assertLess(control.index("panic("), control.index("cqCommand(write ? 49 : 48"))
        self.assertIn("ioswitchSdExtArgument(self->extInfo.perf.fno, address, 1)", control)
        self.assertEqual(len(re.findall(r"cqCommand\(write \? 49|cqCommand\(49|cardCommand\(49", source)), 1)
        card = (DRIVER / "IOSwitchSdCard.h").read_text()
        allowed = card.split("inline uint32_t ioswitchSdPerfWriteAddress(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("case kSdPerfCacheControl: return value == 0 || value == kSdCacheEnable ? cq - 2 : 0;", allowed)
        self.assertIn("case kSdPerfFlushControl: return value == kSdFlushCache ? cq - 1 : 0;", allowed)
        self.assertIn("case kSdPerfCqControl: return value == 0 || value == (kSdCqEnable | kSdCqSequential) ? cq : 0;",
                      allowed)
        self.assertIn("default: return 0;", allowed)
        # Interrupts, never polling: cqCommand waits by waitSignal (a lost interrupt panics); the only timed wait in the
        # queue's code is the engine's settle() between read-backs of a card slow to leave CQ mode.
        command = body("int NativeIOSwitchBlock::cqCommand(")
        self.assertIn("waitSignal(awaited,", command)
        self.assertIn('panic("IOSwitchSD: SDMMC1 interrupt lost', command)
        self.assertNotIn("IOSleep", command)
        self.assertNotIn("IODelay", command)
        # r16-sdcq: a CMD49's busy (the byte applied, a flush done) ends in transfer complete; DAT0 confirms it before
        # the read-back; the limit covers the spec's 1 s of busy or data access.
        self.assertIn("data == kRegWrite && !waitIdle(true, 1000000000)", command)
        self.assertIn("limit = 1500000000ull", command)
        self.assertNotIn("13, ", body("Batch *NativeIOSwitchBlock::runGroup("))  # no CMD13 task-status polling
        engine = (DRIVER / "IOSwitchCq.h").read_text()
        self.assertEqual(engine.count("bus.settle()"), 1)
        self.assertIn("kSdCqEnable | kSdCqSequential", engine)
        # r16-sdcq: every control byte read back lazily (bounded), the enable's value kept for the line.
        self.assertIn("static constexpr unsigned kReadBacks = 10;", engine)
        enter = engine.split("bool enter(Bus &bus)", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("readBack(bus, kSdPerfCqControl, 3, kEnabled)", enter)
        self.assertIn("cqRead = lastRead[2];", enter)
        # The cache only inside a CQ session: on right before 03h, off right after 00h, off with the queue for good.
        self.assertLess(enter.index("enableCache(bus)"), enter.index("bus.control(kSdPerfCqControl, true"))
        leave = engine.split("bool leave(Bus &bus)", 1)[1].split("\n    }\n", 1)[0]
        self.assertLess(leave.index("if (!off(bus))"), leave.index("if (useCache && !disableCache(bus)) useCache = false;"))
        fail = engine.split("bool fail(Bus &bus, const char *reason, bool maybeOn = false)", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("if (out) (void)disableCache(bus);", fail)
        self.assertIn("useCache = false;", fail)
        # Off is proven only by a CMD49 the card answered; no flush on the way (nothing completed is held).
        disable = engine.split("bool disableCache(Bus &bus)", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("if (bus.control(kSdPerfCacheControl, true, &value)) return false;", disable)
        self.assertNotIn("flush(", disable)
        self.assertIn("bool commitWrite(Bus &bus) { return !cacheOn || flush(bus); }", engine)
        cycled = engine.split("void powerCycled()", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("cacheOn = false;", cycled)  # Cache Enable is 0 after power-up
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
        self.assertLess(check.index("cq.start(perf.cqDepth, perf.cache);"), check.index("cq.enter(cqBus)"))
        # r16-sdcq: sessions with the card's cache (enter turns it on first, 4.19.1); a cache found on goes off before any
        # write; a failure says what read back.
        self.assertIn("cq.start(perf.cqDepth, perf.cache);", check)
        self.assertLess(check.index("cq.cacheOn = true;"), check.index("(void)cq.disableCache(cqBus);"))
        self.assertLess(check.index("cq.fail(cqBus, why)"), check.index("describeCq(detail, sizeof(detail));"))

    def test_cache_contract(self):
        """r16-sdcq: write-through, nothing panics. The card's cache is on only inside a CQ session (reads only), so every
        write finds it off and completes on flash as today, synchronize has nothing to flush; a cache that would not go
        off has each write flushed before it completes, and a failed flush fails that write; and the queue only runs
        where its own measurement says it is faster."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        self.assertIn("IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }", source)
        self.assertIn("IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }", source)
        self.assertNotIn("kIOStorageOptionForceUnitAccess", source)  # every completed write is on flash already
        # Every write's last step, after every path that wrote it (direct, bounce, a proof's rewrite).
        end = body("void NativeIOSwitchBlock::endBatch(")
        self.assertIn("if (write && !result && !commitWrite()) result = kIOReturnIOError;", end)
        self.assertLess(end.index("bounceChain(first, sectors)"), end.index("!commitWrite()"))
        self.assertLess(end.index("!commitWrite()"), end.index("b.result = result;"))
        commit = body("bool NativeIOSwitchBlock::commitWrite()")
        self.assertIn("if (!cq.cacheOn) return true;", commit)
        self.assertIn("cq.commitWrite(cqBus)", commit)
        self.assertNotIn("panic(", commit)
        # No panic and no flush on the way to sleep or a power cycle: nothing completed is held in the cache.
        for name in ("void NativeIOSwitchBlock::suspend()", "void NativeIOSwitchBlock::resetCard()",
                     "void NativeIOSwitchBlock::cqStop(", "void NativeIOSwitchBlock::resume()"):
            self.assertNotIn("flush(", body(name))
        self.assertNotIn("panic(", body("void NativeIOSwitchBlock::suspend()"))
        self.assertIsNone(re.search(r"panic\([^;]*(cache|flush)", source, re.IGNORECASE))
        # The queue only where it is faster, and from legacy mode only for groups that pay for the way in and out.
        setup = body("bool NativeIOSwitchBlock::setup()")
        self.assertIn("if (!cqOff && (!saving || need > cq.depth))", setup)
        self.assertIn('(void)cq.fail(cqBus, "no faster");', setup)
        self.assertIn("cqMinChains = need < 2 ? 2 : static_cast<unsigned>(need);", setup)
        self.assertIn("n >= (cq.state == CqEngine::kOn ? 1u : cqMinChains)", body("unsigned NativeIOSwitchBlock::groupLocked("))


if __name__ == "__main__":
    unittest.main()
