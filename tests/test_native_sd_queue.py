import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/NativeIOSwitchBlock"


class NativeSdQueueTests(unittest.TestCase):
    def test_read_chains(self):
        """r12-sdio: queued reads that continue the picked read join its command, wherever they are queued."""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sd-queue"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER), str(ROOT / "tests/native_sd_queue_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: sd queue merging", result.stdout)

    def test_driver_uses_it_for_reads_only(self):
        """Writes keep their FIFO rule; the read chain is checked to tile its command before it goes anywhere."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        pick = source.split("Request *NativeIOSwitchBlock::pickLocked(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("if (merge && mergeAnywhere && !first->write) {", pick)
        self.assertIn("ioswitchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, gap, budget)",
                      pick)
        # r14-sdcq: gaps only while gap reads are on, within the cost model's bounds.
        self.assertIn("const uint32_t gap = gapsOn ? kGapSectors : 0, budget = gapsOn ? kGapBudget : 0;", pick)
        self.assertRegex(source, r"constexpr uint32_t kGapSectors = 32 \* 1024 / kSector, kGapBudget = 64 \* 1024 / kSector")
        # The scan reaches as far as a chain can hold: never fewer than the old rule's queue-adjacent reads.
        self.assertIn("constexpr unsigned kMergeScan = kMaxChain;", source)
        # The chain is checked before it is unlinked; one that fails stays queued, the boot falls back to the old rule
        # with one line, and nothing panics.
        self.assertLess(pick.index("ioswitchChainFits(head, start, sectors, count, gap, budget)"),
                        pick.index("ioswitchUnlinkChain("))
        self.assertIn("ioswitchClearChain(head);", pick)
        self.assertIn("mergeAnywhere = false;", pick)
        self.assertNotIn("panic(", pick)
        # The write path: only the requests right behind, in queue order.
        self.assertIn("last->next->lba == last->lba + last->sectors", pick)
        # Every request starts unchained.
        self.assertRegex(source, r"static_cast<uint8_t>\(priority >> 6\),\s*0,\s*false\};")

    def test_gap_chains(self):
        """r14-sdcq: a read chain's gaps go to the sink and nowhere else; a write chain never has one; the first chains
        with gaps are compared with SDMA before they complete, and a difference turns gap reads off."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        begin = body("void NativeIOSwitchBlock::beginBatch(")
        self.assertIn("if (first->write && payload != sectors)", begin)
        self.assertLess(begin.index("outside the APFS partition"), begin.index("if (first->write && payload != sectors)"))
        table = body("IOSwitchAdmaRefusal NativeIOSwitchBlock::buildTable(")
        self.assertEqual(table.count("staging.addSink(sinkPA, gap(r))"), 1)
        self.assertEqual(table.count("table.addSealed(sinkPA, gap(r))"), 1)
        self.assertEqual(table.count("gap(r) > uint64_t(kGapSectors) * kSector || !sinkPA"), 2)
        end = body("void NativeIOSwitchBlock::endBatch(")
        self.assertIn("} else if (b.gapSectors && gapProofs) {", end)
        prove = body("bool NativeIOSwitchBlock::proveGaps(")
        self.assertIn("gapsOn = false;", prove)
        self.assertIn("compareChain(first, sectors, &at)", prove)
        # compareChain: the requests get the SDMA read's bytes when theirs differ.
        self.assertIn("stageChain(first, 0, sectors * kSector, checkBuffer, true)",
                      body("NativeIOSwitchBlock::ChainProof NativeIOSwitchBlock::compareChain("))
        self.assertIn("(gapsOn && gapProofs)", body("void NativeIOSwitchBlock::releaseChecks()"))
        # Toward the requests gaps are skipped; from them (writes, read-back copies) every byte must be covered.
        self.assertIn("toClient);", body("bool NativeIOSwitchBlock::stageChain("))
        # The cache maintenance and the high-memory proof leave the sink alone.
        self.assertIn("if (pa - skip < skipBytes) continue;", source)
        self.assertIn("|| inSink(pa)) {", body("bool NativeIOSwitchBlock::proveHigh("))


if __name__ == "__main__":
    unittest.main()
