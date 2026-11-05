// SPDX-License-Identifier: BSD-2-Clause-Patent
// r17-launch: the pure parts of IOSwitchCryptex.cpp's direct read map and boot prefetch list, kept free of kernel types
// so the host harness (tests/cryptex_map_harness.cpp) checks this exact code.
//  - extents: the image's bytes as (image offset, SD window offset, length), sorted, contiguous from offset 0;
//  - pieces: a read of the image split into SD reads that each stay inside one extent and one SD command;
//  - runs: logged page ranges of the cryptex files merged into the prefetch's page-in runs.
#ifndef IOSWITCH_CRYPTEX_MAP_H
#define IOSWITCH_CRYPTEX_MAP_H
#include <stdint.h>

struct IOSwitchExtent {
    uint64_t file, disk,
        length;  // bytes; disk is kIOSwitchHole for a range the file does not store (it reads as zeros)
};
constexpr uint64_t kIOSwitchHole = ~0ull;

// Appends an extent, merged into the last one when it continues it in the image and on the card (or both are holes).
// False when full.
inline bool ioswitchAppendExtent(IOSwitchExtent *map, uint32_t *count, uint32_t capacity, uint64_t file, uint64_t disk,
                                 uint64_t length)
{
    const IOSwitchExtent *last = *count ? &map[*count - 1] : nullptr;
    if (last && last->file + last->length == file &&
        (last->disk == kIOSwitchHole ? disk == kIOSwitchHole
                                     : disk != kIOSwitchHole && last->disk + last->length == disk)) {
        map[*count - 1].length += length;
        return true;
    }
    if (*count == capacity) return false;
    map[(*count)++] = {file, disk, length};
    return true;
}

// Whether the extents cover [0, bytes) of the image in order, with nothing missing or overlapping.
inline bool ioswitchExtentsCover(const IOSwitchExtent *map, uint32_t count, uint64_t bytes)
{
    uint64_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (map[i].file != at || !map[i].length) return false;
        at += map[i].length;
    }
    return count && at == bytes;
}

// The extent holding image offset `offset` (the last one that begins at or before it).
inline uint32_t ioswitchExtentAt(const IOSwitchExtent *map, uint32_t count, uint64_t offset)
{
    uint32_t lo = 0, hi = count;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (map[mid].file <= offset)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// The next SD read of an image read now at `offset` with `remaining` bytes left: inside extent *at, at most maxPiece
// bytes (disk kIOSwitchHole: zeros, no read). Advances *at past the extent when the piece ends it. length 0 when the map
// has nothing there.
struct IOSwitchPiece {
    uint64_t disk, length;
};
inline IOSwitchPiece ioswitchNextPiece(const IOSwitchExtent *map, uint32_t count, uint32_t *at, uint64_t offset,
                                       uint64_t remaining, uint64_t maxPiece)
{
    if (*at >= count || offset < map[*at].file || offset - map[*at].file >= map[*at].length) return {0, 0};
    const IOSwitchExtent &e = map[*at];
    const uint64_t within = offset - e.file;
    uint64_t length = e.length - within;
    if (length > remaining) length = remaining;
    if (length > maxPiece) length = maxPiece;
    if (within + length == e.length) ++*at;
    return {e.disk == kIOSwitchHole ? kIOSwitchHole : e.disk + within, length};
}

// A logged read of a cryptex file as pages [first, end), and a page-in run of the list.
struct IOSwitchRange {
    uint64_t fileId;
    uint32_t first, end;
    uint16_t when;  // 1 + first-read second
    uint8_t index;  // 0 System, 1 App
};
struct IOSwitchRun {
    uint64_t fileId;
    uint32_t page;
    uint16_t pages;
    uint8_t when;  // 1 + first-read second, at most 255
    uint8_t index;
};

// Enough runs for any merge of n ranges holding `pages` pages in all.
inline uint64_t ioswitchRunCapacity(uint32_t n, uint64_t pages, uint32_t maxRun, uint32_t maxGap)
{
    return n + (pages + uint64_t(n) * maxGap) / maxRun + 1;
}

// Ranges sorted by (index, fileId, first) to runs: each file's ranges joined through gaps of at most maxGap pages, cut
// into runs of at most maxRun pages, each with the earliest second of what it joined. Returns the runs written; *files
// counts the distinct files.
inline uint32_t ioswitchMergeRuns(const IOSwitchRange *ranges, uint32_t n, IOSwitchRun *runs, uint64_t capacity,
                                  uint32_t maxRun, uint32_t maxGap, uint32_t *files)
{
    uint32_t count = 0;
    *files = 0;
    for (uint32_t i = 0; i < n;) {
        const IOSwitchRange &r = ranges[i];
        uint32_t end = r.end;
        uint16_t when = r.when;
        if (!i || r.fileId != ranges[i - 1].fileId || r.index != ranges[i - 1].index) ++*files;
        for (++i;
             i < n && ranges[i].fileId == r.fileId && ranges[i].index == r.index && ranges[i].first <= end + maxGap;
             ++i) {
            if (ranges[i].end > end) end = ranges[i].end;
            if (ranges[i].when < when) when = ranges[i].when;
        }
        for (uint32_t page = r.first; page < end && count < capacity; page += maxRun) {
            const uint32_t pages = end - page < maxRun ? end - page : maxRun;
            runs[count++] = {r.fileId, page, uint16_t(pages), uint8_t(when < 255 ? when : 255), r.index};
        }
    }
    return count;
}

// The list's replay order: first-read second, then cryptex, file and page.
inline int ioswitchCompareRuns(const IOSwitchRun &x, const IOSwitchRun &y)
{
    if (x.when != y.when) return x.when < y.when ? -1 : 1;
    if (x.index != y.index) return x.index < y.index ? -1 : 1;
    if (x.fileId != y.fileId) return x.fileId < y.fileId ? -1 : 1;
    return x.page < y.page ? -1 : x.page > y.page;
}

inline int ioswitchCompareRanges(const IOSwitchRange &x, const IOSwitchRange &y)
{
    if (x.index != y.index) return x.index < y.index ? -1 : 1;
    if (x.fileId != y.fileId) return x.fileId < y.fileId ? -1 : 1;
    return x.first < y.first ? -1 : x.first > y.first;
}
#endif
