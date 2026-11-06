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

const char kFolder[] = "switchroot/ios/install/21H461";

struct Fat {
    std::map<std::string, std::vector<uint8_t>> files;
    const switch_image *image = nullptr;
    std::string refuse_write;

    explicit Fat(const switch_image &decoded) : image(&decoded)
    {
        for (uint32_t i = 0; i < decoded.part_count; i++) {
            files[decoded.parts[i].path].resize(decoded.parts[i].bytes);
        }
        files[std::string(kFolder) + "/bl31.bin"] = boot_bytes("bl31.bin", 3000);
        files[std::string(kFolder) + "/bl33.bin"] = boot_bytes("bl33.bin", (9u << 20) + 5);
        files["switchroot/ios/bl33.bin"] = {1, 2, 3};
        files["switchroot/ios/bootargs.txt"] = {'-', 'v'};
    }
};

int size(void *context, const char *path, uint64_t *bytes)
{
    auto *fat = static_cast<Fat *>(context);
    auto found = fat->files.find(path);
    if (found == fat->files.end()) {
        return 1;
    }
    *bytes = found->second.size();
    return 0;
}

int read(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes)
{
    auto *fat = static_cast<Fat *>(context);
    auto found = fat->files.find(path);
    if (found == fat->files.end() || at + bytes > found->second.size()) {
        return 1;
    }
    std::memcpy(buffer, found->second.data() + at, bytes);
    return 0;
}

int write(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes)
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
    file.insert(file.end(), static_cast<const uint8_t *>(buffer), static_cast<const uint8_t *>(buffer) + bytes);
    return 0;
}

int verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes)
{
    auto *fat = static_cast<Fat *>(context);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(buffer, bytes, digest);
    return chunk >= fat->image->hash_total ||
           std::memcmp(digest, fat->image->hashes + chunk * SWITCH_IMAGE_HASH_BYTES, sizeof(digest)) != 0;
}

switch_image_state examine(Fat &fat, const switch_image *image, const char *folder, const char **missing)
{
    return switch_image_examine(image, folder, size, &fat, missing);
}

TEST(Examine, AnImageWithEveryFileIsReady)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    const char *missing = "unset";
    EXPECT_EQ(examine(fat, &m.image, kFolder, &missing), SWITCH_IMAGE_READY);
    EXPECT_EQ(missing, nullptr);
}

TEST(Examine, AMissingOrShortPartOrBootFileMakesItIncomplete)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    for (const char *name : {"part-01.bin", "bl31.bin", "bl33.bin"}) {
        std::string path = std::string(kFolder) + "/" + name;
        Fat gone(m.image), short_file(m.image);
        gone.files.erase(path);
        short_file.files[path].pop_back();
        for (Fat *fat : {&gone, &short_file}) {
            const char *missing = nullptr;
            EXPECT_EQ(examine(*fat, &m.image, kFolder, &missing), SWITCH_IMAGE_MISSING) << name;
            EXPECT_STREQ(missing, path.c_str());
        }
    }
}

TEST(Examine, AnImageIsOfferedOnlyFromItsOwnFolder)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    const char *missing;
    EXPECT_EQ(examine(fat, &m.image, "switchroot/ios/install/24B119", &missing), SWITCH_IMAGE_ELSEWHERE);
    EXPECT_EQ(examine(fat, nullptr, kFolder, &missing), SWITCH_IMAGE_UNREADABLE);
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
        {16ull << 20, "16 MB"},    {1023ull << 20, "1023 MB"},  {1ull << 30, "1.0 GB"},
        {64ull << 30, "64.0 GB"},  {64000ull << 20, "62.5 GB"}, {(119ull << 30) + (980ull << 20), "120.0 GB"},
        {1ull << 40, "1024.0 GB"},
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
    switch_boot_io io{&fat, read, write, verify};
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    ASSERT_EQ(switch_boot_check(&m.image, &io, buffer.data()), 0);
    ASSERT_EQ(switch_boot_install(&m.image, &io, buffer.data()), 0);
    EXPECT_EQ(fat.files["switchroot/ios/bl31.bin"], boot_bytes("bl31.bin", 3000));
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"], boot_bytes("bl33.bin", (9u << 20) + 5));
    EXPECT_EQ(fat.files["switchroot/ios/bootargs.txt"], (std::vector<uint8_t>{'-', 'v'}));
    EXPECT_EQ(fat.files.count(std::string(kFolder) + "/bl33.bin"), 1u);
}

TEST(BootFiles, AChunkThatFailsItsHashIsNeverWritten)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    fat.files[std::string(kFolder) + "/bl33.bin"][(8u << 20) + 2] ^= 1;
    switch_boot_io io{&fat, read, write, verify};
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    EXPECT_EQ(switch_boot_check(&m.image, &io, buffer.data()), 2);
    EXPECT_EQ(switch_boot_install(&m.image, &io, buffer.data()), 2);
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"].size(), 8u << 20);
}

TEST(BootFiles, AFailedWriteStops)
{
    Image m;
    ASSERT_EQ(m.decode(), 0);
    Fat fat(m.image);
    fat.refuse_write = "switchroot/ios/bl31.bin";
    switch_boot_io io{&fat, read, write, verify};
    std::vector<uint8_t> buffer(SWITCH_IMAGE_HASH_CHUNK);
    EXPECT_EQ(switch_boot_install(&m.image, &io, buffer.data()), 1);
    EXPECT_EQ(fat.files["switchroot/ios/bl33.bin"], (std::vector<uint8_t>{1, 2, 3}));
}

}  // namespace
