// boot/storage/fat32.c (the loader-side FAT32 reader) over sparse, independently built FAT images (FatImage below,
// no device or mounted disk): fragmented files and case-folded 8.3 paths, reads that never touch the output or the
// device out of range, bounded paths, MBR and BPB validation before data access, mirrored and active FATs, every bad
// chain, finite directory, chain and callback budgets, I/O failures, remounts, 64 KiB clusters and a 4 GB file.
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../fat32.h"
}

namespace {
enum { OK, ARG, IO, FORMAT, RANGE, NOT_FOUND, NOT_FILE, CHAIN, LIMIT, MIRROR };

using Bytes = std::vector<uint8_t>;
using Sector = std::array<uint8_t, 512>;

void Put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}
void Put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

Bytes Dirent(const std::string &name, uint32_t first, uint32_t size = 0, uint8_t attr = 0x20)
{
    Bytes result(32, 0);
    const size_t dot = name.find('.');
    std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    base.resize(8, ' ');
    ext.resize(3, ' ');
    std::memcpy(result.data(), (base + ext).data(), 11);
    result[11] = attr;
    Put16(&result[20], (uint16_t)(first >> 16));
    Put16(&result[26], (uint16_t)(first & 65535));
    Put32(&result[28], size);
    return result;
}

// Independent on-disk fixture; sparse sectors and computed large FAT runs.
struct FatImage {
    uint64_t first = 2048;
    uint32_t spc, clusters, reserved = 32, fats = 2, fat_sectors;
    uint64_t total, capacity;
    Sector mbr{}, bpb{};
    std::map<uint32_t, uint32_t> entries{{0, 0x0ffffff8}, {1, 0x0fffffff}, {2, 0x0fffffff}, {3, 0x0fffffff},
                                         {4, 0x0fffffff}, {5, 7},          {7, 6},          {6, 0x0fffffff}};
    std::map<std::pair<unsigned, uint32_t>, uint32_t> fat_overrides;
    std::map<uint64_t, Sector> sectors;
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    std::vector<uint64_t> reads;
    std::set<uint64_t> failed_lbas;
    std::vector<uint64_t> out_of_bounds;
    std::function<std::optional<Sector>(uint64_t)> synthetic_sector;

    explicit FatImage(uint32_t spcIn = 1, uint32_t clustersIn = 65525) : spc(spcIn), clusters(clustersIn)
    {
        fat_sectors = (clusters + 2 + 127) / 128;
        total = reserved + (uint64_t)fats * fat_sectors + (uint64_t)clusters * spc;
        capacity = first + total + 4096;
        mbr[446 + 4] = 0x0c;
        Put32(&mbr[446 + 8], (uint32_t)first);
        Put32(&mbr[446 + 12], (uint32_t)total);
        mbr[510] = 0x55, mbr[511] = 0xaa;
        bpb[0] = 0xeb, bpb[1] = 0x58, bpb[2] = 0x90;
        Put16(&bpb[11], 512);
        bpb[13] = (uint8_t)spc;
        Put16(&bpb[14], (uint16_t)reserved);
        bpb[16] = (uint8_t)fats;
        bpb[21] = 0xf8;
        Put32(&bpb[28], (uint32_t)first);
        Put32(&bpb[32], (uint32_t)total);
        Put32(&bpb[36], fat_sectors);
        Put32(&bpb[44], 2);
        bpb[510] = 0x55, bpb[511] = 0xaa;
        Directory(2, {Dirent("IOS", 3, 0, 0x10)});
        Directory(3, {Dirent("VERIFY.BIN", 5, 1200), Dirent("OS", 4, 0, 0x10), Dirent("EMPTY.BIN", 0)});
        Directory(4, {Dirent("PART0000.BIN", 5, 1200)});
    }

    uint64_t fat_start() const { return first + reserved; }
    uint64_t data_start() const { return fat_start() + (uint64_t)fats * fat_sectors; }
    uint64_t lba(uint32_t cluster, uint32_t sector = 0) const
    {
        return data_start() + (uint64_t)(cluster - 2) * spc + sector;
    }

    void Directory(uint32_t cluster, const std::vector<Bytes> &list)
    {
        Sector data{};
        size_t at = 0;
        for (const Bytes &e : list) {
            EXPECT_LE(at + e.size(), data.size());
            std::memcpy(data.data() + at, e.data(), e.size());
            at += e.size();
        }
        sectors[lba(cluster)] = data;
    }

    uint32_t Fat(unsigned index, uint32_t cluster) const
    {
        if (auto o = fat_overrides.find({index, cluster}); o != fat_overrides.end()) return o->second;
        if (auto e = entries.find(cluster); e != entries.end()) return e->second;
        for (auto [start, count] : runs)
            if (start <= cluster && cluster < start + count)
                return cluster + 1 < start + count ? cluster + 1 : 0x0fffffff;
        return 0;
    }

    // Incorporate high LBA bits to expose 32-bit byte-offset truncation.
    static Sector Payload(uint64_t lba)
    {
        Sector s;
        for (unsigned i = 0; i < 512; ++i) s[i] = (uint8_t)((lba * 17 + (lba >> 16) + i * 13) & 255);
        return s;
    }

    Sector Read(uint64_t lba) const
    {
        if (lba == 0) return mbr;
        if (lba == first) return bpb;
        if (fat_start() <= lba && lba < data_start()) {
            const uint64_t index = (lba - fat_start()) / fat_sectors, local = (lba - fat_start()) % fat_sectors;
            Sector s;
            for (uint32_t i = 0; i < 128; ++i) Put32(&s[4 * i], Fat((unsigned)index, (uint32_t)(local * 128 + i)));
            return s;
        }
        if (auto s = sectors.find(lba); s != sectors.end()) return s->second;
        if (synthetic_sector)
            if (auto s = synthetic_sector(lba)) return *s;
        return Payload(lba);
    }

    int Callback(uint64_t lba, uint8_t *output)
    {
        reads.push_back(lba);
        if (lba >= capacity || (lba != 0 && !(first <= lba && lba < first + total))) {
            out_of_bounds.push_back(lba);
            return 1;
        }
        if (failed_lbas.count(lba)) {
            std::memset(output, 0xee, 512);  // failure is allowed to scribble into the transport destination
            return 1;
        }
        const Sector data = Read(lba);
        std::memcpy(output, data.data(), 512);
        return 0;
    }
};

// The reader's callback carries no context of ours: the image under test.
FatImage *gImage;
int Read512(void *, uint64_t lba, uint8_t output[512])
{
    return gImage->Callback(lba, output);
}

class NativeFat32 : public testing::Test {
protected:
    std::unique_ptr<FatImage> image = std::make_unique<FatImage>();
    std::unique_ptr<iosw_fat_volume> volume = std::make_unique<iosw_fat_volume>();
    iosw_fat_file file{};

    void SetUp() override { gImage = image.get(); }
    void Replace(std::unique_ptr<FatImage> next)
    {
        image = std::move(next);
        gImage = image.get();
    }

    void Mount(int expected = OK, std::optional<uint64_t> capacity = std::nullopt, uint32_t partition = 0)
    {
        EXPECT_EQ(iosw_fat_mount(volume.get(), Read512, nullptr, capacity ? *capacity : image->capacity, partition),
                  expected);
        EXPECT_TRUE(image->out_of_bounds.empty());
    }
    int Open(const char *path) { return iosw_fat_open(volume.get(), path, &file); }

    // Reads `count` bytes into a buffer with a guard byte on each side; returns what landed between them.
    Bytes Read(uint64_t offset, uint32_t count, int expected = OK)
    {
        Bytes buffer(count + 2, 0xaa);
        EXPECT_EQ(iosw_fat_read(volume.get(), &file, offset, buffer.data() + 1, count), expected);
        EXPECT_EQ(buffer.front(), 0xaa);
        EXPECT_EQ(buffer.back(), 0xaa);
        EXPECT_TRUE(image->out_of_bounds.empty());
        return Bytes(buffer.begin() + 1, buffer.end() - 1);
    }
};

Bytes Concat(std::initializer_list<Sector> parts)
{
    Bytes out;
    for (const Sector &s : parts) out.insert(out.end(), s.begin(), s.end());
    return out;
}
}  // namespace

TEST_F(NativeFat32, FragmentedPartialSectorExactFileAndCasefoldedPath)
{
    Mount();
    EXPECT_EQ(volume->cluster_count, 65525u);
    ASSERT_EQ(Open("/ios/verify.bin"), OK);
    Bytes expected =
        Concat({FatImage::Payload(image->lba(5)), FatImage::Payload(image->lba(7)), FatImage::Payload(image->lba(6))});
    expected.resize(1200);
    EXPECT_EQ(Read(499, 600), Bytes(expected.begin() + 499, expected.begin() + 1099));
    EXPECT_EQ(Read(0, 1200), expected);
    EXPECT_EQ(Read(1200, 0), Bytes());
    EXPECT_EQ(file.size, 1200u);
}

TEST_F(NativeFat32, InvalidReadsDoNotTouchOutputOrReadDevice)
{
    Mount();
    ASSERT_EQ(Open("IOS/VERIFY.BIN"), OK);
    const size_t before = image->reads.size();
    for (auto [offset, n] : {std::pair<uint64_t, uint32_t>{1201, 0}, {1200, 1}, {1199, 2}, {UINT64_MAX, 1}})
        EXPECT_EQ(Read(offset, n, RANGE), Bytes(n, 0xaa)) << offset;
    EXPECT_EQ(image->reads.size(), before);
}

TEST_F(NativeFat32, EmptyFileAndNonFile)
{
    Mount();
    ASSERT_EQ(Open("IOS/EMPTY.BIN"), OK);
    Read(0, 0);
    Read(0, 1, RANGE);
    EXPECT_EQ(Open("IOS/OS"), NOT_FILE);
    EXPECT_EQ(Open("IOS/VERIFY.BIN/X"), NOT_FILE);
}

TEST_F(NativeFat32, PathsAreBoundedAnd83Only)
{
    Mount();
    const std::vector<std::string> invalid{"",
                                           "/",
                                           "//IOS",
                                           "IOS/",
                                           "IOS//VERIFY.BIN",
                                           "../VERIFY.BIN",
                                           "IOS/./VERIFY.BIN",
                                           "IOS/VERIFY.",
                                           "IOS/PART-0000.BIN",
                                           "IOS/VERI*.BIN",
                                           std::string(105, 'A')};
    const size_t before = image->reads.size();
    for (const std::string &path : invalid) EXPECT_EQ(Open(path.c_str()), ARG) << '"' << path << '"';
    EXPECT_EQ(Open("A/A/A/A/A/A/A/A/A"), LIMIT);
    EXPECT_EQ(image->reads.size(), before);
}

TEST_F(NativeFat32, MbrCapacityAndSignatureChecks)
{
    Mount(ARG, std::nullopt, 4);
    Mount(ARG, 0);
    Mount(RANGE, image->first + image->total - 1);
    for (auto [offset, value, expected] :
         {std::tuple<size_t, uint8_t, int>{510, 0, FORMAT}, {450, 0x83, FORMAT}, {446, 1, FORMAT}}) {
        const uint8_t original = image->mbr[offset];
        image->mbr[offset] = value;
        Mount(expected);
        image->mbr[offset] = original;
    }
    Put32(&image->mbr[454], 0);
    Mount(RANGE);
}

TEST_F(NativeFat32, TrustedSliceAnd64BitPartitionAddressing)
{
    image->first = (1ull << 32) + 12345;
    image->capacity = image->first + image->total + 4096;
    // Rebuild directory locations after moving the partition above 2TiB.
    image->sectors.clear();
    image->Directory(2, {Dirent("IOS", 3, 0, 0x10)});
    image->Directory(3, {Dirent("VERIFY.BIN", 5, 1200)});
    EXPECT_EQ(iosw_fat_mount_slice(volume.get(), Read512, nullptr, image->capacity, image->first, image->total), OK);
    EXPECT_EQ(image->reads, std::vector<uint64_t>{image->first}) << "MBR not consulted";
    ASSERT_EQ(Open("IOS/VERIFY.BIN"), OK);
    const Sector payload = FatImage::Payload(image->lba(5));
    EXPECT_EQ(Read(0, 512), Bytes(payload.begin(), payload.end()));
    for (auto [first, count] : {std::pair<uint64_t, uint64_t>{0, 10},
                                {image->first, 0},
                                {image->capacity, 1},
                                {UINT64_MAX, 2},
                                {image->first, image->total - 1},
                                {image->first, UINT64_MAX}})
        EXPECT_EQ(iosw_fat_mount_slice(volume.get(), Read512, nullptr, image->capacity, first, count), RANGE)
            << first << " + " << count;
    EXPECT_TRUE(image->out_of_bounds.empty());
}

TEST_F(NativeFat32, BpbGeometryIsValidatedBeforeDataAccess)
{
    struct Case {
        size_t offset;
        unsigned width;
        uint32_t value;
        int expected;
    };
    const std::vector<Case> cases{{11, 2, 1024, FORMAT},
                                  {13, 1, 0, FORMAT},
                                  {13, 1, 3, FORMAT},
                                  {14, 2, 0, FORMAT},
                                  {16, 1, 0, FORMAT},
                                  {16, 1, 3, FORMAT},
                                  {17, 2, 1, FORMAT},
                                  {19, 2, 123, FORMAT},
                                  {22, 2, 1, FORMAT},
                                  {32, 4, 0, FORMAT},
                                  {32, 4, (uint32_t)image->total + 1, RANGE},
                                  {36, 4, 0, FORMAT},
                                  {36, 4, 1, FORMAT},
                                  {40, 2, 0x10, FORMAT},
                                  {40, 2, 0x82, FORMAT},
                                  {42, 2, 1, FORMAT},
                                  {44, 4, 1, FORMAT},
                                  {44, 4, image->clusters + 2, FORMAT},
                                  {510, 2, 0, FORMAT}};
    const Sector original = image->bpb;
    for (const Case &c : cases) {
        image->bpb = original;
        for (unsigned i = 0; i < c.width; ++i) image->bpb[c.offset + i] = (uint8_t)(c.value >> (8 * i));
        SCOPED_TRACE(testing::Message() << "BPB offset " << c.offset << " = " << c.value);
        Mount(c.expected);
    }
    image->bpb = original;
    Put32(&image->bpb[32], (uint32_t)image->total - 1);
    Mount(FORMAT);  // 65524 clusters is FAT16, regardless of label.
    for (uint64_t lba : image->reads) EXPECT_TRUE(lba == 0 || lba == image->first) << lba;
}

TEST_F(NativeFat32, MirrorDisagreementAndReservedHighBits)
{
    image->fat_overrides[{1, 7}] = 8;
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), MIRROR);
    image->fat_overrides[{1, 7}] = 0xa0000006;
    image->entries[5] |= 0xf0000000;
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), OK);
}

TEST_F(NativeFat32, ActiveNonmirroredFatIsSelected)
{
    Put16(&image->bpb[40], 0x81);
    for (uint32_t cluster : {2u, 3u, 5u, 7u, 6u}) {
        image->fat_overrides[{1, cluster}] = image->entries[cluster];
        image->entries[cluster] = 0x0ffffff7;
    }
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), OK);
    for (uint64_t lba : image->reads)
        EXPECT_FALSE(image->fat_start() <= lba && lba < image->fat_start() + image->fat_sectors) << lba;
}

TEST_F(NativeFat32, BadFreeReservedOutOfRangeAndShortLongCyclicFileChains)
{
    for (uint32_t value : {0u, 1u, 0x0ffffff0u, 0x0ffffff7u, image->clusters + 2, 0x0fffffffu, 5u, 7u}) {
        image->entries[7] = value;
        Mount();
        EXPECT_EQ(Open("IOS/VERIFY.BIN"), CHAIN) << "FAT[7] = " << value;
    }
    image->entries[7] = 6;
    image->entries[6] = 8;
    image->entries[8] = 0x0fffffff;
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), CHAIN);
}

TEST_F(NativeFat32, DirectorySkipsDeletedLfnVolumeAndStopsAtZero)
{
    Bytes deleted = Dirent("VERIFY.BIN", 8, 512);
    deleted[0] = 0xe5;
    image->Directory(3, {deleted, Dirent("VERIFY.BIN", 8, 512, 0x0f), Dirent("VERIFY.BIN", 8, 512, 8),
                         Dirent("VERIFY.BIN", 5, 1200)});
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), OK);
    image->Directory(3, {Bytes(32, 0), Dirent("VERIFY.BIN", 5, 1200)});
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), NOT_FOUND);
}

namespace {
Sector DeletedEntries()
{
    Sector s{};
    for (unsigned i = 0; i < 16; ++i) s[32 * i] = 0xe5;
    return s;
}
}  // namespace

TEST_F(NativeFat32, FragmentedDirectoryChainAndCycle)
{
    image->sectors[image->lba(3)] = DeletedEntries();
    image->entries[3] = 129;
    image->entries[129] = 0x0fffffff;
    image->Directory(129, {Dirent("VERIFY.BIN", 5, 1200)});
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), OK);
    image->entries[129] = 3;
    image->sectors[image->lba(129)] = image->sectors[image->lba(3)];
    Mount();
    const size_t before = image->reads.size();
    EXPECT_EQ(Open("IOS/MISSING.BIN"), CHAIN);
    EXPECT_LT(image->reads.size() - before, 16u);
}

TEST_F(NativeFat32, DirectoryHasFiniteSectorBudget)
{
    image->entries[3] = 100;
    image->runs = {{100, 5000}};
    const Sector deleted = DeletedEntries();
    image->sectors[image->lba(3)] = deleted;
    FatImage *im = image.get();
    image->synthetic_sector = [im, deleted](uint64_t lba) -> std::optional<Sector> {
        if (im->lba(100) <= lba && lba < im->lba(5100)) return deleted;
        return std::nullopt;
    };
    Mount();
    const size_t before = image->reads.size();
    EXPECT_EQ(Open("IOS/MISSING.BIN"), LIMIT);
    EXPECT_LT(image->reads.size() - before, 4200u);
}

TEST_F(NativeFat32, FileChainAndCallbackBudgetsAreFinite)
{
    Replace(std::make_unique<FatImage>(1, 1048600));
    image->Directory(3, {Dirent("HUGE.BIN", 100, 1048577u * 512u)});
    Mount();
    size_t before = image->reads.size();
    EXPECT_EQ(Open("IOS/HUGE.BIN"), LIMIT);
    EXPECT_LT(image->reads.size() - before, 10u);

    // An otherwise valid operation cannot make an unlimited number of sector callbacks, even when its requested file
    // range is in bounds.
    Replace(std::make_unique<FatImage>());
    const uint32_t size = 33000 * 512;
    image->runs = {{100, 33000}};
    image->Directory(3, {Dirent("LARGE.BIN", 100, size)});
    Sector x;
    x.fill('x');
    image->synthetic_sector = [x](uint64_t) -> std::optional<Sector> { return x; };
    Mount();
    ASSERT_EQ(Open("IOS/LARGE.BIN"), OK);
    before = image->reads.size();
    const Bytes output = Read(0, size, LIMIT);
    EXPECT_EQ(image->reads.size() - before, 32768u);
    EXPECT_EQ(Bytes(output.begin(), output.begin() + 512), Bytes(512, 'x'));
    EXPECT_EQ(Bytes(output.end() - 512, output.end()), Bytes(512, 0xaa));
}

TEST_F(NativeFat32, IoFailureMountOpenReadAndOutputPrefixContract)
{
    image->failed_lbas = {0};
    Mount(IO);
    image->failed_lbas = {image->first};
    Mount(IO);
    image->failed_lbas = {image->fat_start()};
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), IO);
    image->failed_lbas = {image->lba(7)};
    Mount();
    ASSERT_EQ(Open("IOS/VERIFY.BIN"), OK);
    const Bytes output = Read(0, 1024, IO);
    const Sector first = FatImage::Payload(image->lba(5));
    EXPECT_EQ(Bytes(output.begin(), output.begin() + 512), Bytes(first.begin(), first.end()));
    EXPECT_EQ(Bytes(output.begin() + 512, output.end()), Bytes(512, 0xaa));
}

TEST_F(NativeFat32, SuccessfulAndFailedRemountInvalidateOldFilesAndCache)
{
    Mount();
    ASSERT_EQ(Open("IOS/VERIFY.BIN"), OK);
    Mount();
    Read(0, 1, ARG);
    image->entries[7] = 0;
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), CHAIN);
    image->entries[7] = 6;
    Mount();
    EXPECT_EQ(Open("IOS/VERIFY.BIN"), OK);
    Mount(ARG, std::nullopt, 4);
    Read(0, 1, ARG);
}

TEST_F(NativeFat32, SixtyFourKiBClustersAndFourBillionByteFile)
{
    Replace(std::make_unique<FatImage>(128, 131100));
    const uint32_t size = 4000000000u, first = 100, count = (uint32_t)((size + 65535ull) / 65536);
    image->runs = {{first, count}};
    image->Directory(3, {Dirent("LARGE.BIN", first, size)});
    Mount();
    ASSERT_EQ(Open("IOS/LARGE.BIN"), OK);
    EXPECT_LT(image->reads.size(), 1000u) << "cached FAT sectors, not one read per link";
    const uint64_t offset = size - 513;
    Bytes expected;
    for (uint64_t pos = offset; pos < size; ++pos)
        expected.push_back(FatImage::Payload(image->lba(first) + pos / 512)[pos % 512]);
    EXPECT_EQ(Read(offset, 513), expected);
}
