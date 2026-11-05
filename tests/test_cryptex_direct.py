import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TREE = ROOT / "."
DRIVER = TREE / "drivers/NativeIOSwitchBlock"


def body(source, signature):
    return source.split(signature, 1)[1].split("\n}\n", 1)[0]


class CryptexDirectTests(unittest.TestCase):
    """r17-launch: the cryptex images are read straight from the SD card into the requesting pages (IOSwitchCryptex.cpp's
    direct read map) instead of through DiskImages and the image files' page cache, and the boot prefetch pages the
    cryptex volumes' files in."""

    def test_map_and_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "cryptex-map"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER), str(ROOT / "tests/cryptex_map_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=400)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: cryptex map", result.stdout)

    def test_reads_go_to_the_card_without_a_copy(self):
        source = (DRIVER / "IOSwitchCryptex.cpp").read_text()
        read = body(source, "IOReturn IOSwitchCryptexDevice::doAsyncReadWrite(")
        # Each piece is a sub-range of the request's own memory, issued on the SD device: no buffer, no copy.
        self.assertIn("ioswitchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece)", read)
        self.assertIn("IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn)", read)
        self.assertIn("sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, attributes,", read)
        for copy in ("readBytes", "memcpy", "bcopy", "copypv", "VNOP_READ", "vn_rdwr"):
            self.assertNotIn(copy, read)
        # The only bytes the CPU writes into a request: zeros where a sparse image file stores nothing.
        self.assertEqual(read.count("writeBytes("), 1)
        self.assertIn("buffer->writeBytes(done + k, gZeros, kImageBlock)", read)
        self.assertIn("return kIOReturnNotWritable;", read)
        # Completions reach the client from a thread call, never from the SD driver's worker.
        done = body(source, "void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)")
        self.assertIn("thread_call_enter(p->call);", done)
        self.assertNotIn("client.action", done)
        self.assertIn("client.action(client.target, client.parameter, status, bytes);",
                      body(source, "void finishRequest(thread_call_param_t parameter, thread_call_param_t)"))

    def test_not_a_virtual_device_and_read_only(self):
        """The cryptex APFS must mount without MNTK_VIRTUALDEV (clustered, cluster-layer-verified page-ins): the device
        reports SD storage, never the virtual interconnect DiskImages reports."""
        source = (DRIVER / "IOSwitchCryptex.cpp").read_text()
        configure = body(source, "bool IOSwitchCryptexDevice::configure(")
        self.assertIn('kIOPropertyPhysicalInterconnectTypeKey, OSString::withCStringNoCopy("SD")', configure)
        self.assertNotIn("Virtual", configure)
        self.assertNotIn("kIOPropertyPhysicalInterconnectTypeVirtual", source)
        self.assertRegex(source, r"reportWriteProtection\(bool \*value\) override\s*\{\s*\*value = true;")

    def test_map_checked_before_use(self):
        """Extents resolved once, on the SD APFS window only, covering the image; sampled blocks compared both ways; a
        refused map attaches through DiskImages with one line; a failed mount falls back the same way."""
        source = (DRIVER / "IOSwitchCryptex.cpp").read_text()
        resolve = body(source, "Extent *resolveExtents(")
        self.assertIn("VNOP_BLOCKMAP(image,", resolve)
        self.assertIn("DKIOCGETPHYSICALEXTENT", resolve)
        self.assertIn("request.dev != whole", resolve)
        self.assertIn("ioswitchAppendExtent(map, &n, kMaxExtents, at, kIOSwitchHole, run)", resolve)
        self.assertIn("ioswitchExtentsCover(map, n, bytes)", resolve)
        attach = body(source, "IOService *attachDirect(unsigned index, dev_t preboot, const char **why)")
        self.assertIn("UF_COMPRESSED", attach)
        self.assertIn("sameBothWays(image, map[0], 0, sd, direct, file)", attach)
        self.assertIn("sameBothWays(image, last, va.va_data_size - kImageBlock, sd, direct, file)", attach)
        entry = body(source, 'extern "C" int ioswitch_cryptex_attach(')
        self.assertIn("direct read map refused", entry)
        self.assertIn("attachDiskImages(index)", entry)
        mount = (TREE / "diagnostics/ioswitch/IOSwitchRawMount.inc").read_text()
        self.assertIn("ioswitch_cryptex_fallback(i, &image_volume, path, sizeof(path))", mount)
        self.assertIn("ioswitch_cryptex_mounted(i, cryptex_mount);", mount)

    def test_prefetch_pages_files_in(self):
        """The prefetch replays page-ins of the cryptex files (absent pages, the files' own pages), bounded in flight;
        the old image-block replay through advisory reads of the image file is gone."""
        source = (DRIVER / "IOSwitchCryptex.cpp").read_text()
        thread = body(source, "void prefetchThread(void *, wait_result_t)")
        self.assertIn("vnode_pagein(vp, nullptr, 0,", thread)
        self.assertIn("while (gFlow.inflight >= kInFlightBytes)", thread)
        self.assertIn("IOLockSleep(gFlow.lock, &gFlow.inflight, THREAD_UNINT);", thread)
        self.assertNotIn("advisory_read", source)
        self.assertIn("VFS_VGET(gPrefetch.mounts[run.index]", thread)
        names = body(source, "int nameVnode(vnode_t vp, void *argument)")
        self.assertIn("hit->vid != vnode_vid(vp)", names)
        self.assertIn("vnode_iterate(gPrefetch.mounts[index], 0, nameVnode, &naming)",
                      body(source, "uint32_t buildRuns("))


if __name__ == "__main__":
    unittest.main()
