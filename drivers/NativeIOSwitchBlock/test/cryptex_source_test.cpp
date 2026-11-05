// r17-launch: guards on IOSwitchCryptex.cpp (cryptex_map_test.cpp tests the map it uses; the driver is not built for the
// host). The cryptex images are read straight from the SD card into the requesting pages (the direct read map) instead
// of through DiskImages and the image files' page cache, and the boot prefetch pages the cryptex volumes' files in.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using ioswitch::test::Body;
using ioswitch::test::Count;
using ioswitch::test::Search;
using ioswitch::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &cryptex()
{
    static const std::string source = SourceText("../IOSwitchCryptex.cpp");
    return source;
}
std::string body(const char *signature)
{
    return Body(cryptex(), signature);
}
}  // namespace

TEST(CryptexSource, ReadsGoToTheCardWithoutACopy)
{
    const std::string read = body("IOReturn IOSwitchCryptexDevice::doAsyncReadWrite(");
    // Each piece is a sub-range of the request's own memory, issued on the SD device: no buffer, no copy.
    EXPECT_THAT(read, HasSubstr("ioswitchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece)"));
    EXPECT_THAT(read, HasSubstr("IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn)"));
    EXPECT_THAT(read,
                HasSubstr("sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, attributes,"));
    for (const char *copy : {"readBytes", "memcpy", "bcopy", "copypv", "VNOP_READ", "vn_rdwr"})
        EXPECT_THAT(read, Not(HasSubstr(copy)));
    // The only bytes the CPU writes into a request: zeros where a sparse image file stores nothing.
    EXPECT_EQ(Count(read, "writeBytes("), 1u);
    EXPECT_THAT(read, HasSubstr("buffer->writeBytes(done + k, gZeros, kImageBlock)"));
    EXPECT_THAT(read, HasSubstr("return kIOReturnNotWritable;"));
    // Completions reach the client from a thread call, never from the SD driver's worker.
    const std::string done = body("void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)");
    EXPECT_THAT(done, HasSubstr("thread_call_enter(p->call);"));
    EXPECT_THAT(done, Not(HasSubstr("client.action")));
    EXPECT_THAT(body("void finishRequest(thread_call_param_t parameter, thread_call_param_t)"),
                HasSubstr("client.action(client.target, client.parameter, status, bytes);"));
}

// The cryptex APFS must mount without MNTK_VIRTUALDEV (clustered, cluster-layer-verified page-ins): the device reports
// SD storage, never the virtual interconnect DiskImages reports.
TEST(CryptexSource, NotAVirtualDeviceAndReadOnly)
{
    const std::string &source = cryptex();
    const std::string configure = body("bool IOSwitchCryptexDevice::configure(");
    EXPECT_THAT(configure, HasSubstr("kIOPropertyPhysicalInterconnectTypeKey, OSString::withCStringNoCopy(\"SD\")"));
    EXPECT_THAT(configure, Not(HasSubstr("Virtual")));
    EXPECT_THAT(source, Not(HasSubstr("kIOPropertyPhysicalInterconnectTypeVirtual")));
    EXPECT_TRUE(Search(source, R"(reportWriteProtection\(bool \*value\) override\s*\{\s*\*value = true;)"));
}

// Extents resolved once, on the SD APFS window only, covering the image; sampled blocks compared both ways; a refused
// map attaches through DiskImages with one line; a failed mount falls back the same way.
TEST(CryptexSource, MapCheckedBeforeUse)
{
    const std::string resolve = body("Extent *resolveExtents(");
    EXPECT_THAT(resolve, HasSubstr("VNOP_BLOCKMAP(image,"));
    EXPECT_THAT(resolve, HasSubstr("DKIOCGETPHYSICALEXTENT"));
    EXPECT_THAT(resolve, HasSubstr("request.dev != whole"));
    EXPECT_THAT(resolve, HasSubstr("ioswitchAppendExtent(map, &n, kMaxExtents, at, kIOSwitchHole, run)"));
    EXPECT_THAT(resolve, HasSubstr("ioswitchExtentsCover(map, n, bytes)"));
    const std::string attach = body("IOService *attachDirect(unsigned index, dev_t preboot, const char **why)");
    EXPECT_THAT(attach, HasSubstr("UF_COMPRESSED"));
    EXPECT_THAT(attach, HasSubstr("sameBothWays(image, map[0], 0, sd, direct, file)"));
    EXPECT_THAT(attach, HasSubstr("sameBothWays(image, last, va.va_data_size - kImageBlock, sd, direct, file)"));
    const std::string entry = body("extern \"C\" int ioswitch_cryptex_attach(");
    EXPECT_THAT(entry, HasSubstr("direct read map refused"));
    EXPECT_THAT(entry, HasSubstr("attachDiskImages(index)"));
    const std::string mount = SourceText("../../../diagnostics/ioswitch/IOSwitchRawMount.inc");
    EXPECT_THAT(mount, HasSubstr("ioswitch_cryptex_fallback(i, &image_volume, path, sizeof(path))"));
    EXPECT_THAT(mount, HasSubstr("ioswitch_cryptex_mounted(i, cryptex_mount);"));
}

// The prefetch replays page-ins of the cryptex files (absent pages, the files' own pages), bounded in flight; the old
// image-block replay through advisory reads of the image file is gone.
TEST(CryptexSource, PrefetchPagesFilesIn)
{
    const std::string thread = body("void prefetchThread(void *, wait_result_t)");
    EXPECT_THAT(thread, HasSubstr("vnode_pagein(vp, nullptr, 0,"));
    EXPECT_THAT(thread, HasSubstr("while (gFlow.inflight >= kInFlightBytes)"));
    EXPECT_THAT(thread, HasSubstr("IOLockSleep(gFlow.lock, &gFlow.inflight, THREAD_UNINT);"));
    EXPECT_THAT(cryptex(), Not(HasSubstr("advisory_read")));
    EXPECT_THAT(thread, HasSubstr("VFS_VGET(gPrefetch.mounts[run.index]"));
    EXPECT_THAT(body("int nameVnode(vnode_t vp, void *argument)"), HasSubstr("hit->vid != vnode_vid(vp)"));
    EXPECT_THAT(body("uint32_t buildRuns("),
                HasSubstr("vnode_iterate(gPrefetch.mounts[index], 0, nameVnode, &naming)"));
}
