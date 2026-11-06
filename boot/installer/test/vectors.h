// The install image manifest_vectors.py wrote, as the installer's tests load it.
#ifndef SWITCH_INSTALL_TEST_VECTORS_H
#define SWITCH_INSTALL_TEST_VECTORS_H

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include "boot/installer/manifest.h"
}

namespace switch_test {

inline std::vector<uint8_t> load(const char *name)
{
    std::ifstream in(std::string(SWITCH_TEST_PACKAGE) + "/" + name, std::ios::binary);
    EXPECT_TRUE(in) << name;
    return {std::istreambuf_iterator<char>(in), {}};
}

constexpr uint64_t kMiB = uint64_t(1) << 20;
constexpr uint64_t kMinimum = 16 * kMiB;

// manifest_vectors.py's boot_bytes().
inline std::vector<uint8_t> boot_bytes(const char *name, size_t size)
{
    unsigned seed = 0;
    for (const char *c = name; *c; c++) {
        seed += static_cast<uint8_t>(*c);
    }
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i++) {
        bytes[i] = static_cast<uint8_t>(i * 31 + seed);
    }
    return bytes;
}

struct Image {
    std::vector<uint8_t> raw = load("manifest.bin");
    std::vector<switch_image_extent> extents = std::vector<switch_image_extent>(16);
    switch_image image{};

    int decode()
    {
        return switch_image_decode(raw.data(), raw.size(), &image, extents.data(), extents.size());
    }

    // The CRC again after an edit, so only the edit's own rule can refuse it.
    void reseal()
    {
        std::memset(raw.data() + 64, 0, 4);
        uint32_t crc = switch_crc32(0, raw.data(), static_cast<uint32_t>(raw.size()));
        std::memcpy(raw.data() + 64, &crc, 4);
    }

    void setPath(size_t record, const char *path)
    {
        std::memset(raw.data() + record, 0, SWITCH_IMAGE_PATH_BYTES);
        std::memcpy(raw.data() + record, path, std::strlen(path));
        reseal();
    }
};

}  // namespace switch_test

#endif
