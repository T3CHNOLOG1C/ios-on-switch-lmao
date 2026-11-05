// r17-launch: IOSwitchCryptexMap.h, the direct read map's request splitting and the boot prefetch list's run merging,
// against brute-force models on random maps, requests and logs. What IOSwitchCryptex.cpp does with them is guarded by
// cryptex_source_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "../IOSwitchCryptexMap.h"

// Millions of checks: the first 20 failures of a test are reported (as GoogleTest failures, with their line), the rest
// counted; a test passes only with none.
static unsigned long failures;
static void report(const char *file, int line, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void report(const char *file, int line, const char *format, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    ADD_FAILURE_AT(file, line) << text;
}
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            if (failures++ < 20) report(__FILE__, __LINE__, __VA_ARGS__); \
        }                                                                 \
    } while (0)

// One random stream for both tests, in order, as when they were one program: the logs' cases follow the maps'.
static std::mt19937_64 rng(17);

TEST(CryptexMap, RequestsSplitIntoPiecesOfOneExtent)
{
    failures = 0;
    const uint64_t kBlock = 4096;
    unsigned long reads = 0, pieces = 0;

    // Maps: images of random fragmentation; their extents through ioswitchAppendExtent (adjacent ones merge).
    for (int round = 0; round < 3000; ++round) {
        const uint32_t parts = 1 + rng() % 40;
        std::vector<IOSwitchExtent> map(parts + 1);
        std::vector<uint64_t> diskOf;  // the SD offset of every image block (the model)
        uint32_t count = 0;
        uint64_t file = 0, disk = (rng() % 1000) * 512;
        for (uint32_t i = 0; i < parts; ++i) {
            const uint64_t length = (1 + rng() % 600) * kBlock;
            if (rng() % 7 == 0) {  // a hole (sparse file): zeros, no card read
                CHECK(ioswitchAppendExtent(map.data(), &count, parts, file, kIOSwitchHole, length), "append refused\n");
                for (uint64_t b = 0; b < length / kBlock; ++b) diskOf.push_back(kIOSwitchHole);
                file += length;
                continue;
            }
            if (rng() % 3) disk += (1 + rng() % 5000) * 512;  // else contiguous with the previous part: merged
            CHECK(ioswitchAppendExtent(map.data(), &count, parts, file, disk, length), "append refused\n");
            for (uint64_t b = 0; b < length / kBlock; ++b) diskOf.push_back(disk + b * kBlock);
            file += length;
            disk += length;
        }
        CHECK(ioswitchExtentsCover(map.data(), count, file), "map does not cover\n");
        CHECK(!ioswitchExtentsCover(map.data(), count, file + kBlock), "cover accepts a short map\n");
        for (uint32_t i = 1; i < count; ++i) {
            const bool holes = map[i - 1].disk == kIOSwitchHole && map[i].disk == kIOSwitchHole;
            const bool joined = map[i - 1].disk != kIOSwitchHole && map[i].disk != kIOSwitchHole &&
                                map[i - 1].disk + map[i - 1].length == map[i].disk;
            CHECK(!holes && !joined, "adjacent extents not merged\n");
        }
        // Requests: every piece inside one extent and one SD read, pieces tiling the request in order, each block where
        // the model puts it.
        const uint64_t blocks = file / kBlock;
        for (int q = 0; q < 50; ++q) {
            const uint64_t start = rng() % blocks, n = 1 + rng() % std::min<uint64_t>(blocks - start, 300);
            const uint64_t maxPiece = (rng() % 2 ? 2048 : 512) * 512ull;
            uint32_t at = ioswitchExtentAt(map.data(), count, start * kBlock);
            uint64_t done = 0;
            while (done < n * kBlock) {
                const IOSwitchPiece piece =
                    ioswitchNextPiece(map.data(), count, &at, start * kBlock + done, n * kBlock - done, maxPiece);
                CHECK(piece.length && piece.length <= maxPiece && piece.length % kBlock == 0, "bad piece length\n");
                if (!piece.length) break;
                for (uint64_t b = 0; b < piece.length / kBlock; ++b) {
                    const uint64_t want = diskOf[start + done / kBlock + b];
                    CHECK(piece.disk == kIOSwitchHole ? want == kIOSwitchHole : piece.disk + b * kBlock == want,
                          "block mapped wrong\n");
                }
                // inside one extent
                const uint32_t e = ioswitchExtentAt(map.data(), count, start * kBlock + done);
                CHECK(start * kBlock + done + piece.length <= map[e].file + map[e].length, "piece crosses extents\n");
                done += piece.length;
                ++pieces;
            }
            CHECK(done == n * kBlock, "request not tiled\n");
            ++reads;
        }
    }

    EXPECT_EQ(failures, 0u) << "checks failed (the first 20 reported above)";
    printf("cryptex map: %lu reads in %lu pieces\n", reads, pieces);
}

TEST(CryptexMap, PrefetchLogsMergeIntoRuns)
{
    failures = 0;
    unsigned long logs = 0;
    // Logs: random reads of a few files; merged runs must hold every logged page no later than its first read, stay
    // in one file, stay within maxRun pages, not overlap, and hold nothing farther than maxGap from a logged page.
    const uint32_t kMaxRun = 128, kMaxGap = 8;
    for (int round = 0; round < 2000; ++round) {
        const uint32_t n = 1 + rng() % 400;
        std::vector<IOSwitchRange> ranges(n);
        uint64_t pages = 0;
        for (auto &r : ranges) {
            r.index = uint8_t(rng() % 2);
            r.fileId = 100 + rng() % 6;
            r.first = uint32_t(rng() % 4000);
            r.end = r.first + 1 + uint32_t(rng() % (rng() % 4 ? 16 : 600));
            r.when = uint16_t(1 + rng() % 300);
            pages += r.end - r.first;
        }
        std::sort(ranges.begin(), ranges.end(),
                  [](const IOSwitchRange &x, const IOSwitchRange &y) { return ioswitchCompareRanges(x, y) < 0; });
        const uint64_t capacity = ioswitchRunCapacity(n, pages, kMaxRun, kMaxGap);
        std::vector<IOSwitchRun> runs(capacity);
        uint32_t files = 0;
        const uint32_t count = ioswitchMergeRuns(ranges.data(), n, runs.data(), capacity, kMaxRun, kMaxGap, &files);
        CHECK(count < capacity, "capacity reached (%u of %llu)\n", count, (unsigned long long)capacity);
        std::set<std::pair<uint64_t, uint8_t>> distinct;
        for (auto &r : ranges) distinct.insert({r.fileId, r.index});
        CHECK(files == distinct.size(), "files %u, expected %zu\n", files, distinct.size());
        for (uint32_t i = 0; i < count; ++i) CHECK(runs[i].pages && runs[i].pages <= kMaxRun, "run size\n");
        for (auto &r : ranges) {
            for (uint32_t page = r.first; page < r.end; ++page) {
                unsigned holding = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    const IOSwitchRun &x = runs[i];
                    if (x.fileId == r.fileId && x.index == r.index && x.page <= page && page < x.page + x.pages) {
                        ++holding;
                        CHECK(x.when <= (r.when < 255 ? r.when : 255), "run later than its read\n");
                    }
                }
                CHECK(holding == 1, "page %u of file %llu in %u runs\n", page, (unsigned long long)r.fileId, holding);
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            const IOSwitchRun &x = runs[i];
            for (uint32_t page = x.page; page < x.page + x.pages; ++page) {
                bool near = false;
                for (auto &r : ranges)
                    near |= r.fileId == x.fileId && r.index == x.index && page + kMaxGap >= r.first &&
                            page < r.end + kMaxGap;
                CHECK(near, "run page %u far from any read\n", page);
            }
        }
        std::sort(runs.begin(), runs.begin() + count,
                  [](const IOSwitchRun &x, const IOSwitchRun &y) { return ioswitchCompareRuns(x, y) < 0; });
        for (uint32_t i = 1; i < count; ++i) CHECK(runs[i - 1].when <= runs[i].when, "replay order\n");
        ++logs;
    }
    EXPECT_EQ(failures, 0u) << "checks failed (the first 20 reported above)";
    printf("cryptex map: %lu prefetch logs merged\n", logs);
}

// The prefetch list's file format IOSWPF02, frozen. A build whose loader refuses the list an earlier build saved makes
// its first boot a recording boot with no prefetch. So this list, laid out byte by byte as IOSwitchCryptex.cpp writes
// it, must keep loading: a new format comes with a loader that still reads (converts) this one, and this test is
// changed to load both.
namespace {
void put(std::vector<uint8_t> &bytes, size_t at, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) bytes[at + i] = uint8_t(value >> (8 * i));  // little-endian, as the device
}
}  // namespace

TEST(CryptexMap, TheSavedListFormatStaysReadable)
{
    // The layout.
    EXPECT_EQ(sizeof(IOSwitchListHeader), 96u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, magic), 0u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, version), 8u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, headerBytes), 12u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, images), 16u);
    EXPECT_EQ(sizeof(IOSwitchImageKey), 32u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, runs), 80u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, files), 84u);
    EXPECT_EQ(offsetof(IOSwitchListHeader, checksum), 88u);
    EXPECT_EQ(sizeof(IOSwitchRun), 16u);
    EXPECT_EQ(offsetof(IOSwitchRun, fileId), 0u);
    EXPECT_EQ(offsetof(IOSwitchRun, page), 8u);
    EXPECT_EQ(offsetof(IOSwitchRun, pages), 12u);
    EXPECT_EQ(offsetof(IOSwitchRun, when), 14u);
    EXPECT_EQ(offsetof(IOSwitchRun, index), 15u);
    // The checksum: FNV-1a 64 (its published vectors).
    EXPECT_EQ(ioswitchFnv(reinterpret_cast<const uint8_t *>(""), 0), 0xcbf29ce484222325ull);
    EXPECT_EQ(ioswitchFnv(reinterpret_cast<const uint8_t *>("a"), 1), 0xaf63dc4c8601ec8cull);

    // A saved list: header, then two runs.
    std::vector<uint8_t> file(96 + 2 * 16, 0);
    for (unsigned i = 0; i < 8; ++i) file[i] = uint8_t("IOSWPF02"[i]);
    put(file, 8, 2, 4);
    put(file, 12, 96, 4);
    const IOSwitchImageKey keys[2] = {{3303014400ull, 0x1d2e3full, 1730800000, 123456789},
                                      {10485760ull, 0x4a5bull, 1730800001, 987654321}};
    for (unsigned i = 0; i < 2; ++i) {
        put(file, 16 + 32 * i, keys[i].bytes, 8);
        put(file, 24 + 32 * i, keys[i].fileId, 8);
        put(file, 32 + 32 * i, uint64_t(keys[i].mtimeSec), 8);
        put(file, 40 + 32 * i, uint64_t(keys[i].mtimeNsec), 8);
    }
    put(file, 80, 2, 4);             // runs
    put(file, 84, 1, 4);             // files
    put(file, 96, 0x1000123ull, 8);  // run 0: file id, page, pages, when, index
    put(file, 104, 4096, 4);
    put(file, 108, 128, 2);
    file[110] = 3;
    file[111] = 0;
    put(file, 112, 0x1000123ull, 8);
    put(file, 120, 4224, 4);
    put(file, 124, 17, 2);
    file[126] = 9;
    file[127] = 0;
    put(file, 88, ioswitchFnv(file.data() + 96, 32), 8);

    IOSwitchListHeader header;
    memcpy(&header, file.data(), sizeof(header));
    EXPECT_EQ(ioswitchListRefusal(header, keys, 1u << 18), nullptr) << "a saved IOSWPF02 list no longer loads";
    std::vector<IOSwitchRun> runs(2);
    memcpy(runs.data(), file.data() + 96, 32);
    EXPECT_EQ(ioswitchFnv(reinterpret_cast<const uint8_t *>(runs.data()), 32), header.checksum);
    EXPECT_EQ(runs[1].fileId, 0x1000123ull);
    EXPECT_EQ(runs[1].page, 4224u);
    EXPECT_EQ(runs[1].pages, 17u);
    EXPECT_EQ(runs[1].when, 9u);

    // What it refuses, and says.
    IOSwitchListHeader other = header;
    other.images[1].mtimeNsec += 1;
    EXPECT_STREQ(ioswitchListRefusal(other, keys, 1u << 18), "saved for other cryptex images");
    other = header;
    other.magic[7] = '1';
    EXPECT_STREQ(ioswitchListRefusal(other, keys, 1u << 18), "not an IOSWPF02 list");
    other = header;
    other.version = 3;
    EXPECT_STREQ(ioswitchListRefusal(other, keys, 1u << 18), "an IOSWPF02 list of another version");
    other = header;
    other.runs = 0;
    EXPECT_STREQ(ioswitchListRefusal(other, keys, 1u << 18), "no runs, or too many");
    EXPECT_STREQ(ioswitchListRefusal(header, keys, 1), "no runs, or too many");
}
