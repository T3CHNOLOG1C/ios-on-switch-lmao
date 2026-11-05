// r17-launch: IOSwitchCryptexMap.h, the direct read map's request splitting and the boot prefetch list's run merging,
// against brute-force models on random maps, requests and logs.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

#include "IOSwitchCryptexMap.h"

static unsigned long failures;
#define CHECK(cond, ...)                              \
    do {                                              \
        if (!(cond)) {                                \
            if (failures++ < 20) printf(__VA_ARGS__); \
        }                                             \
    } while (0)

int main()
{
    std::mt19937_64 rng(17);
    const uint64_t kBlock = 4096;
    unsigned long reads = 0, pieces = 0, logs = 0;

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
    if (failures) {
        printf("FAIL: %lu checks failed\n", failures);
        return 1;
    }
    printf("PASS: cryptex map: %lu reads in %lu pieces, %lu prefetch logs merged\n", reads, pieces, logs);
    return 0;
}
