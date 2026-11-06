// The index tools/install/overlay_index.py writes, built in memory for the tests (same layout, no checks).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace switch_pt_test {

struct File {
    std::string path;
    uint64_t system_id;
    uint64_t data_id;
    std::array<uint8_t, 20> cdhash{};
};

inline void put32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void put64(std::vector<uint8_t> &out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline std::vector<uint8_t> encode(const std::vector<File> &files, const std::vector<uint64_t> &directories)
{
    std::string strings;
    std::vector<uint32_t> offsets;
    for (const File &f : files) {
        offsets.push_back(static_cast<uint32_t>(strings.size()));
        strings += f.path;
        strings.push_back('\0');
    }
    std::vector<uint8_t> out;
    out.insert(out.end(), {'S', 'W', 'O', 'V'});
    put32(out, 2);
    put32(out, static_cast<uint32_t>(files.size()));
    put32(out, static_cast<uint32_t>(directories.size()));
    put32(out, static_cast<uint32_t>(strings.size()));
    put32(out, 0);
    for (size_t i = 0; i < files.size(); ++i) {
        put64(out, files[i].system_id);
        put64(out, files[i].data_id);
        put32(out, offsets[i]);
        put32(out, 0);
        out.insert(out.end(), files[i].cdhash.begin(), files[i].cdhash.end());
    }
    for (uint64_t d : directories)
        put64(out, d);
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

inline void set32(std::vector<uint8_t> &raw, size_t at, uint32_t v)
{
    std::memcpy(raw.data() + at, &v, sizeof(v));
}

inline void set64(std::vector<uint8_t> &raw, size_t at, uint64_t v)
{
    std::memcpy(raw.data() + at, &v, sizeof(v));
}

}  // namespace switch_pt_test
