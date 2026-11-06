// install-ios.bin's decoding, partition choice and write stream on the host, against an image the Mac's own
// tools/install/manifest.py wrote (manifest_vectors.py).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "boot/installer/test/vectors.h"

extern "C" {
#include "boot/installer/stream.h"
#include "boot/installer/target.h"
}

namespace {

using switch_test::Image;
using switch_test::load;

TEST(Manifest, DecodesWhatTheMacWrote)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    EXPECT_EQ(m.image.partition_bytes, 16u << 20);
    EXPECT_EQ(m.image.block_size, 4096u);
    EXPECT_EQ(m.image.part_count, 2u);
    EXPECT_STREQ(m.image.parts[1].path, "switchroot/ios/install/21H461/part-01.bin");
    EXPECT_EQ(m.image.extent_count, 3u);
    EXPECT_EQ(m.image.extents[1].source, SWITCH_IMAGE_ZERO);
    EXPECT_EQ(m.image.hash_count, 2u);
    EXPECT_STREQ(m.image.label, "iPadOS 17.7.11 (21H461)");
    ASSERT_EQ(m.image.boot_count, 2u);
    EXPECT_STREQ(m.image.boot[0].path, "switchroot/ios/install/21H461/bl31.bin");
    EXPECT_EQ(m.image.boot[0].bytes, 3000u);
    EXPECT_EQ(m.image.boot[0].first_hash, 2u);
    EXPECT_EQ(m.image.boot[1].bytes, (9u << 20) + 5);
    EXPECT_EQ(m.image.boot[1].first_hash, 3u);
    EXPECT_EQ(m.image.hash_total, 5u);
}

TEST(Manifest, KnowsItsOwnFolderWhateverTheCase)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    EXPECT_TRUE(switch_image_in(&m.image, "switchroot/ios/install/21H461"));
    EXPECT_TRUE(switch_image_in(&m.image, "SWITCHROOT/ios/install/21h461"));
    EXPECT_FALSE(switch_image_in(&m.image, "switchroot/ios/install"));
    EXPECT_FALSE(switch_image_in(&m.image, "switchroot/ios/install/24B119"));
    EXPECT_FALSE(switch_image_in(&m.image, "switchroot/ios/install/21H461/"));
}

TEST(Manifest, RefusesTheFirstVersion)
{
    Image m;
    m.raw[8] = 1;
    m.reseal();
    EXPECT_NE(m.decode(), 0);
}

TEST(Manifest, RefusesBootFilesOutsideTheImageOrOverTheUsersFiles)
{
    for (const char *path : {"switchroot/ios/install/24B119/bl31.bin", "switchroot/ios/install/21H461/bootargs.txt",
                             "switchroot/ios/install/21H461/manifest.bin", "switchroot/ios/install/21H461/part-00.bin",
                             "bl31.bin", "switchroot/ios/install/21H461/../bl31.bin"}) {
        Image m;
        m.set_path(SWITCH_IMAGE_HEADER_BYTES + 2 * SWITCH_IMAGE_RECORD_BYTES, path);
        EXPECT_NE(m.decode(), 0) << path;
    }
    Image renamed;
    renamed.set_path(SWITCH_IMAGE_HEADER_BYTES + 2 * SWITCH_IMAGE_RECORD_BYTES,
                     "switchroot/ios/install/21H461/bootlogo.bmp");
    EXPECT_EQ(renamed.decode(), 0);
}

TEST(Manifest, RefusesAnyChangedByte)
{
    Image clean;
    for (size_t at : {size_t(8), size_t(20), size_t(130), clean.raw.size() - 30, clean.raw.size() - 1}) {
        Image m;
        m.raw[at] ^= 0x40;
        EXPECT_NE(m.decode(), 0) << "byte " << at;
    }
}

TEST(Manifest, RefusesShortBuffers)
{
    Image m;
    m.raw.pop_back();
    EXPECT_NE(m.decode(), 0);
    Image small;
    EXPECT_NE(switch_image_decode(small.raw.data(), small.raw.size(), &small.image, small.extents.data(), 2), 0);
}

struct Device {
    explicit Device(const std::vector<uint8_t> &bytes) : source(bytes)
    {}

    const std::vector<uint8_t> &source;
    std::vector<uint8_t> partition = std::vector<uint8_t>(16u << 20, 0xA5);
    uint64_t first = 2048, read_at = 0;
    int bad_chunk = -1;
    std::vector<uint32_t> verified;
};

int read(void *context, void *buffer, uint32_t bytes)
{
    auto *d = static_cast<Device *>(context);
    if (d->read_at + bytes > d->source.size()) {
        return 1;
    }
    std::memcpy(buffer, d->source.data() + d->read_at, bytes);
    d->read_at += bytes;
    return 0;
}

int verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes)
{
    auto *d = static_cast<Device *>(context);
    d->verified.push_back(chunk);
    uint64_t at = uint64_t(chunk) * SWITCH_IMAGE_HASH_CHUNK;
    return int(chunk) == d->bad_chunk || std::memcmp(buffer, d->source.data() + at, bytes) != 0;
}

int write(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
    auto *d = static_cast<Device *>(context);
    if (sector < d->first || (sector - d->first + count) * 512 > d->partition.size()) {
        return 1;
    }
    std::memcpy(d->partition.data() + (sector - d->first) * 512, buffer, count * 512);
    return 0;
}

int stream(Device &d, const switch_image &image)
{
    switch_install_io io{&d, read, verify, write, nullptr};
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK), zeros(1u << 20);
    return switch_install_stream(&image, &io, d.first, d.partition.size() / 512, buffer.data(), zeros.data(),
                                 zeros.size());
}

TEST(Stream, LeavesThePartitionTheMacPlanned)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    std::vector<uint8_t> source = load("source.bin"), expected = load("expected.bin");
    Device d(source);
    ASSERT_EQ(stream(d, m.image), 0);
    EXPECT_EQ(d.verified, (std::vector<uint32_t>{0, 1}));
    EXPECT_TRUE(d.partition == expected);
}

TEST(Stream, WritesNothingOfAChunkThatFailsItsHash)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    std::vector<uint8_t> source = load("source.bin");
    Device d(source);
    d.bad_chunk = 0;
    EXPECT_NE(stream(d, m.image), 0);
    EXPECT_TRUE(std::all_of(d.partition.begin(), d.partition.end(), [](uint8_t b) {
        return b == 0xA5;
    }));
}

TEST(Stream, RefusesAPartitionOfAnotherSize)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    std::vector<uint8_t> source = load("source.bin");
    Device d(source);
    d.partition.resize(d.partition.size() + 4096, 0xA5);
    EXPECT_NE(stream(d, m.image), 0);
    EXPECT_EQ(d.read_at, 0u);
}

TEST(Stream, RefusesASourceWithoutTheContainerSuperblock)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    std::vector<uint8_t> source = load("source.bin");
    source[33] ^= 1;
    Device d(source);
    EXPECT_NE(stream(d, m.image), 0);
    EXPECT_TRUE(std::all_of(d.partition.begin(), d.partition.end(), [](uint8_t b) {
        return b == 0xA5;
    }));
}

// A Switch card as hekate's partition manager leaves it: hos_data, Linux l4t, Android's userdata, emuMMC.
struct Card {
    switch_gpt gpt{};

    void entry(uint32_t index, const uint8_t *type, uint64_t first, uint64_t last, const char *name)
    {
        uint8_t *e = gpt.entries + index * SWITCH_GPT_ENTRY_BYTES;
        std::memcpy(e, type, 16);
        e[16] = uint8_t(index + 1);
        std::memcpy(e + 32, &first, 8);
        std::memcpy(e + 40, &last, 8);
        for (size_t i = 0; name[i]; i++) {
            e[56 + 2 * i] = uint8_t(name[i]);
        }
    }

    Card()
    {
        static const uint8_t fat[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
        static const uint8_t emummc[16] = {0x00, 0x7e, 0xca, 0x11, 0,    0,    0,    0,
                                           0,    0,    0x65, 0x6d, 0x75, 0x4d, 0x4d, 0x43};
        gpt.disk_sectors = 1u << 24;
        gpt.entry_count = 128;
        gpt.table_sectors = 32;
        entry(0, fat, 0x8000, 0x7fffff, "hos_data");
        entry(1, switch_gpt_linux, 0x800000, 0xbfffff, "l4t");
        entry(2, switch_gpt_linux, 0xc00000, 0xdfffff, "userdata");
        entry(3, emummc, 0xe00000, 0xefffff, "emummc");
        std::memcpy(gpt.backup_entries, gpt.entries, sizeof(gpt.entries));
    }

    int targets(switch_target *out)
    {
        return switch_targets(&gpt, 0x8000, out, SWITCH_TARGET_MAX);
    }
};

TEST(Targets, OffersLinuxAndAndroidNeverTheFatOrEmummc)
{
    Card card;
    switch_target t[SWITCH_TARGET_MAX];
    ASSERT_EQ(card.targets(t), 2);
    EXPECT_STREQ(t[0].name, "l4t");
    EXPECT_EQ(t[0].first, 0x800000u);
    EXPECT_EQ(t[0].sectors, 0x400000u);
    EXPECT_STREQ(t[1].name, "userdata");
}

TEST(Targets, AnInstalledOrHalfInstalledPartitionIsTheOnlyChoice)
{
    for (const char *name : {"iOS", "iOS-setup"}) {
        Card card;
        switch_gpt_set(&card.gpt, 2, std::string(name) == "iOS" ? switch_gpt_apfs : switch_gpt_linux, name);
        switch_target t[SWITCH_TARGET_MAX];
        ASSERT_EQ(card.targets(t), 1) << name;
        EXPECT_EQ(t[0].index, 2u);
        EXPECT_TRUE(t[0].installed);
        EXPECT_STREQ(t[0].name, name);
    }
    Card two;
    switch_gpt_set(&two.gpt, 1, switch_gpt_apfs, "iOS");
    switch_gpt_set(&two.gpt, 2, switch_gpt_apfs, "iOS");
    switch_target t[SWITCH_TARGET_MAX];
    EXPECT_EQ(two.targets(t), -1);
}

TEST(Targets, RetypingChangesOneEntryInBothCopies)
{
    Card card;
    std::vector<uint8_t> before(card.gpt.entries, card.gpt.entries + sizeof(card.gpt.entries));
    switch_gpt_set(&card.gpt, 1, switch_gpt_apfs, "iOS");
    EXPECT_EQ(std::memcmp(card.gpt.entries, card.gpt.backup_entries, sizeof(card.gpt.entries)), 0);
    for (size_t i = 0; i < before.size(); i++) {
        bool inside = i >= 128 && i < 256 && (i - 128 < 16 || i - 128 >= 56);
        if (!inside) {
            EXPECT_EQ(card.gpt.entries[i], before[i]) << i;
        }
    }
    EXPECT_EQ(std::memcmp(card.gpt.entries + 128, switch_gpt_apfs, 16), 0);
    EXPECT_EQ(card.gpt.entries[128 + 56], 'i');
    EXPECT_EQ(card.gpt.entries[128 + 62], 0);
}

}  // namespace
