// install-ios.bin's image list (which images it offers, how the buttons move through them) and its copy of the chosen
// image's boot files into hekate's boot entry, on the host against manifest_vectors.py's image.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "boot/installer/test/vectors.h"

extern "C" {
#include "boot/installer/bootfiles.h"
#include "boot/installer/images.h"
}

namespace {

using switch_test::boot_bytes;
using switch_test::Image;

const char kFirst[] = "switchroot/ios/IOS17.SII.001";

// The FAT as a map of path to bytes, and the image's stream, whose boot region holds the boot files.
struct Fat {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<uint64_t, std::vector<uint8_t>> boot;
    const switch_image *image = nullptr;
    std::string refuse_write;

    explicit Fat(const switch_image &decoded) : image(&decoded)
    {
        for (uint32_t i = 0; i < decoded.part_count; i++) {
            files[decoded.parts[i].path].resize(decoded.parts[i].bytes);
        }
        for (uint32_t i = 0; i < decoded.boot_count; i++) {
            boot[decoded.boot[i].offset] = boot_bytes(decoded.boot[i].name, decoded.boot[i].bytes);
        }
        files["switchroot/ios/bl33.bin"] = {1, 2, 3};
        files["switchroot/ios/bootargs.txt"] = {'-', 'v'};
    }
};

int size(void *context, const char *path, uint64_t *bytes)
{
    const auto *fat = static_cast<const Fat *>(context);
    auto found = fat->files.find(path);
    if (found == fat->files.end()) {
        return 1;
    }
    *bytes = found->second.size();
    return 0;
}

int read_stream(void *context, uint64_t at, void *buffer, uint32_t bytes)
{
    const auto *fat = static_cast<const Fat *>(context);
    auto file = fat->boot.upper_bound(at);
    if (file == fat->boot.begin()) {
        return 1;
    }
    --file;
    if (at + bytes > file->first + file->second.size()) {
        return 1;
    }
    std::memcpy(buffer, file->second.data() + (at - file->first), bytes);
    return 0;
}

int read_file(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes)
{
    const auto *fat = static_cast<const Fat *>(context);
    auto found = fat->files.find(path);
    if (found == fat->files.end() || at + bytes > found->second.size()) {
        return 1;
    }
    std::memcpy(buffer, found->second.data() + at, bytes);
    return 0;
}

int write_file(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes)
{
    auto *fat = static_cast<Fat *>(context);
    if (path == fat->refuse_write) {
        return 1;
    }
    auto &file = fat->files[path];
    if (!at) {
        file.clear();
    }
    if (at != file.size()) {
        return 1;
    }
    const auto *data = static_cast<const uint8_t *>(buffer);
    file.insert(file.end(), data, data + bytes);
    return 0;
}

int verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes)
{
    const auto *fat = static_cast<const Fat *>(context);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(buffer, bytes, digest);
    return static_cast<int>(chunk >= fat->image->hash_total ||
                            std::memcmp(digest, fat->image->hashes + (size_t(chunk) * SWITCH_IMAGE_HASH_BYTES),
                                        sizeof(digest)) != 0);
}

switch_boot_io io(Fat &fat)
{
    return {&fat, read_stream, read_file, write_file, verify};
}

switch_image_state examine(Fat &fat, const switch_image *image, const char *path, const char **missing)
{
    return switch_image_examine(image, path, size, &fat, missing);
}

TEST(Examine, AnImageWithEveryPartIsReady)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    const char *missing = "unset";
    EXPECT_EQ(examine(fat, &m.image, kFirst, &missing), SWITCH_IMAGE_READY);
    EXPECT_EQ(missing, nullptr);
}

TEST(Examine, AMissingOrShortPartMakesItIncomplete)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    for (const char *path : {"switchroot/ios/IOS17.SII.001", "switchroot/ios/IOS17.SII.002"}) {
        Fat gone(m.image);
        Fat short_part(m.image);
        gone.files.erase(path);
        short_part.files[path].pop_back();
        for (Fat *fat : {&gone, &short_part}) {
            const char *missing = nullptr;
            EXPECT_EQ(examine(*fat, &m.image, kFirst, &missing), SWITCH_IMAGE_MISSING) << path;
            EXPECT_STREQ(missing, path);
        }
    }
}

TEST(Examine, ARenamedFirstPartIsNotOffered)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    const char *missing = nullptr;
    EXPECT_EQ(examine(fat, &m.image, "switchroot/ios/IOS27.SII.001", &missing), SWITCH_IMAGE_ELSEWHERE);
    EXPECT_EQ(examine(fat, nullptr, kFirst, &missing), SWITCH_IMAGE_UNREADABLE);
}

TEST(Menu, MovesOnlyBetweenReadyRowsAndWraps)
{
    const uint8_t ready[] = {0, 1, 0, 1, 1};
    uint32_t current = switch_menu_first(ready, 5);
    EXPECT_EQ(current, 1u);
    EXPECT_EQ(switch_menu_press(ready, 5, &current, SWITCH_KEY_VOL_DOWN), SWITCH_MENU_MOVED);
    EXPECT_EQ(current, 3u);
    switch_menu_press(ready, 5, &current, SWITCH_KEY_VOL_DOWN);
    EXPECT_EQ(current, 4u);
    switch_menu_press(ready, 5, &current, SWITCH_KEY_VOL_DOWN);
    EXPECT_EQ(current, 1u);
    switch_menu_press(ready, 5, &current, SWITCH_KEY_VOL_UP);
    EXPECT_EQ(current, 4u);
    EXPECT_EQ(switch_menu_press(ready, 5, &current, SWITCH_KEY_POWER), SWITCH_MENU_CHOSEN);
    EXPECT_EQ(current, 4u);
    EXPECT_EQ(switch_menu_press(ready, 5, &current, SWITCH_KEY_VOL_UP | SWITCH_KEY_VOL_DOWN), SWITCH_MENU_CANCELLED);
}

TEST(Menu, NothingReadyIsNeverChosen)
{
    const uint8_t ready[] = {0, 0};
    uint32_t current = switch_menu_first(ready, 2);
    EXPECT_EQ(current, 2u);
    EXPECT_EQ(switch_menu_press(ready, 2, &current, SWITCH_KEY_POWER), SWITCH_MENU_MOVED);
    EXPECT_EQ(current, 2u);
}

TEST(Menu, OneRowStaysPut)
{
    const uint8_t ready[] = {1};
    uint32_t current = 0;
    switch_menu_press(ready, 1, &current, SWITCH_KEY_VOL_UP);
    EXPECT_EQ(current, 0u);
    switch_menu_press(ready, 1, &current, SWITCH_KEY_VOL_DOWN);
    EXPECT_EQ(current, 0u);
}

TEST(SizeText, BinaryUnitsLikeHekate)
{
    char text[SWITCH_SIZE_TEXT];
    const std::pair<uint64_t, const char *> cases[] = {
        {16ULL << 20, "16 MB"},    {1023ULL << 20, "1023 MB"},  {1ULL << 30, "1.0 GB"},
        {64ULL << 30, "64.0 GB"},  {64000ULL << 20, "62.5 GB"}, {(119ULL << 30) + (980ULL << 20), "120.0 GB"},
        {1ULL << 40, "1024.0 GB"},
    };
    for (const auto &[bytes, want] : cases) {
        switch_size_text(bytes, text);
        EXPECT_STREQ(text, want) << bytes;
    }
}

TEST(BootFiles, GoIntoTheBootEntryAndLeaveTheUsersFilesAlone)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    const switch_boot_io boot_io = io(fat);
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    ASSERT_EQ(switch_boot_check(&m.image, &boot_io, buffer.data()), 0);
    ASSERT_EQ(switch_boot_install(&m.image, &boot_io, buffer.data()), 0);
    EXPECT_EQ(fat.files["switchroot/ios/bl31.bin"], boot_bytes("bl31.bin", 3000));
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"], boot_bytes("bl33.bin", (9 * switch_test::kMiB) + 5));
    EXPECT_EQ(fat.files["switchroot/ios/bootargs.txt"], (std::vector<uint8_t>{'-', 'v'}));
}

TEST(BootFiles, AChunkThatFailsItsHashIsNeverWritten)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    fat.boot[m.image.boot[1].offset][(8 * switch_test::kMiB) + 2] ^= 1;
    const switch_boot_io boot_io = io(fat);
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    EXPECT_EQ(switch_boot_check(&m.image, &boot_io, buffer.data()), 2);
    EXPECT_EQ(switch_boot_install(&m.image, &boot_io, buffer.data()), 2);
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"].size(), 8 * switch_test::kMiB);
}

TEST(BootFiles, AFailedWriteStops)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    fat.refuse_write = "switchroot/ios/bl31.bin";
    const switch_boot_io boot_io = io(fat);
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    EXPECT_EQ(switch_boot_install(&m.image, &boot_io, buffer.data()), 1);
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"], (std::vector<uint8_t>{1, 2, 3}));
}

}  // namespace
