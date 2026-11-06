#include "export.h"
#include "signatures.h"

/* This native profile excludes the retained firmware region from XNU. Do
 * not accept object roots in general DRAM or in the excluded final 3MiB. */
#define DRAM_LOW    UINT64_C(0xf0000000)
#define DRAM_END    UINT64_C(0xffd00000)
#define HEAP_BYTES  UINT64_C(0x402000)
#define IMAGE_BYTES 0x68000u
#define MAX_CLASSES 128u
#define MAX_DEVICES 128u

static uint32_t u32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint64_t u64(const uint8_t *b)
{
    return (uint64_t)u32(b) | (uint64_t)u32(b + 4) << 32;
}

static void p32(uint8_t *b, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        b[i] = (uint8_t)(v >> (8 * i));
}

static void p64(uint8_t *b, uint64_t v)
{
    p32(b, (uint32_t)v);
    p32(b + 4, (uint32_t)(v >> 32));
}

static uint32_t crc32(const uint8_t *b, uint32_t n)
{
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= b[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int span(uint64_t p, uint32_t bytes, uint64_t low, uint64_t end)
{
    return !(p & 7) && p >= low && p < end && bytes <= end - p;
}

static int read_ram(const struct switch_fw_reader *r, uint64_t p, uint8_t *out, uint32_t bytes, uint64_t low,
                    uint64_t end)
{
    return span(p, bytes, low, end) && r->copy(r->context, p, out, bytes) == 0;
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int snapshot(const struct switch_fw_reader *r, uint64_t gd, uint8_t out[256])
{
    uint8_t g[0xb0], word[8], uc[0x30], dev[0xa0], desc[0x88];
    uint8_t parent[0xa0], mmc[0xe8], priv[8], up[8];
    if (!read_ram(r, gd, g, sizeof g, DRAM_LOW, DRAM_END))
        return SWITCH_FW_GLOBAL_DATA;
    uint64_t base = u64(g + 0x58), reloc = u64(g + 0x80);
    if ((u64(g + 8) & 0x201) != 0x201 || !u64(g + 0x90) || (base & 0xfff) || base < DRAM_LOW + HEAP_BYTES ||
        !span(base, IMAGE_BYTES, DRAM_LOW, DRAM_END) || base - SWITCH_FW_IMAGE_LINK != reloc)
        return SWITCH_FW_GLOBAL_DATA;
    /* The exact instructions establish every pointer root, offset and
     * cached-card interpretation used below. Instruction reads cannot
     * enter controller MMIO: base has already been bounded to DRAM. */
    for (unsigned i = 0; i < sizeof switch_fw_words / sizeof switch_fw_words[0]; ++i) {
        uint64_t address = base + switch_fw_words[i].offset;
        if (r->copy(r->context, address, word, 4) || u32(word) != switch_fw_words[i].word)
            return SWITCH_FW_SIGNATURE;
    }
    /* mem_malloc_start/end are BSS globals from the exact binary. Both must
     * match initr_malloc's fixed 0x402000-byte allocation below relocaddr. */
    if (!read_ram(r, base + 0x66cb0, word, 8, base, base + IMAGE_BYTES) || u64(word) != base - HEAP_BYTES)
        return SWITCH_FW_HEAP;
    if (!read_ram(r, base + 0x66ca8, word, 8, base, base + IMAGE_BYTES) || u64(word) != base)
        return SWITCH_FW_HEAP;
    uint64_t low = base - HEAP_BYTES, end = base;
    uint64_t head = gd + 0xa0, node = u64(g + 0xa0), previous = head, block_class = 0;
    unsigned classes = 0;
    while (node != head) {
        if (++classes > MAX_CLASSES || node < 0x20 || !read_ram(r, node - 0x20, uc, sizeof uc, low, end) ||
            u64(uc + 0x28) != previous)
            return SWITCH_FW_CLASSES;
        if (u64(uc + 8) == base + 0x523f0) {
            if (block_class)
                return SWITCH_FW_CLASSES;
            block_class = node - 0x20;
        }
        previous = node;
        node = u64(uc + 0x20);
    }
    if (!block_class || previous != u64(g + 0xa8))
        return SWITCH_FW_CLASSES;
    if (!read_ram(r, block_class, uc, sizeof uc, low, end))
        return SWITCH_FW_CLASSES;
    head = block_class + 0x10;
    node = u64(uc + 0x10);
    previous = head;
    uint64_t tail = u64(uc + 0x18), found_dev = 0, found_desc = 0;
    unsigned devices = 0;
    while (node != head) {
        if (++devices > MAX_DEVICES || node < 0x60 || !read_ram(r, node - 0x60, dev, sizeof dev, low, end) ||
            u64(dev + 0x68) != previous || u64(dev + 0x48) != block_class)
            return SWITCH_FW_DEVICES;
        uint64_t dp = u64(dev + 0x20);
        if (!read_ram(r, dp, desc, sizeof desc, low, end))
            return SWITCH_FW_DEVICES;
        if (u32(desc) == 6 && u32(desc + 4) == 1) {
            if (found_dev || u64(dev) != base + 0x51c10 || !(u32(dev + 0x90) & 1) || u64(desc + 0x80) != node - 0x60 ||
                desc[0xb] != 0)
                return SWITCH_FW_DEVICES;
            found_dev = node - 0x60;
            found_desc = dp;
        }
        previous = node;
        node = u64(dev + 0x60);
    }
    if (!found_dev || previous != tail || !read_ram(r, found_dev, dev, sizeof dev, low, end) ||
        !read_ram(r, found_desc, desc, sizeof desc, low, end))
        return SWITCH_FW_DEVICES;
    uint64_t pp = u64(dev + 0x38);
    if (!read_ram(r, pp, parent, sizeof parent, low, end) || u64(parent) != base + 0x52048 || !(u32(parent + 0x90) & 1))
        return SWITCH_FW_CARD_LINKS;
    uint64_t plat = u64(parent + 0x10), private = u64(parent + 0x40), uprivate = u64(parent + 0x50);
    if (!span(plat, 0x108, low, end) || !read_ram(r, private, priv, sizeof priv, low, end) || u64(priv) != 0x700b0000 ||
        !read_ram(r, uprivate, up, sizeof up, low, end))
        return SWITCH_FW_CARD_LINKS;
    uint64_t mp = u64(up);
    if (mp != plat + 0x20 || !read_ram(r, mp, mmc, sizeof mmc, low, end) || u64(mmc) != plat || u64(mmc + 0x10) != pp ||
        u64(mmc + 0xe0) != pp)
        return SWITCH_FW_CARD_LINKS;
    uint32_t version = u32(mmc + 8), ocr = u32(mmc + 0x2c), csd0 = u32(mmc + 0x40);
    uint32_t rca = (uint32_t)mmc[0x60] | (uint32_t)mmc[0x61] << 8;
    if (u32(mmc + 0x18) != 1 || u32(mmc + 0x1c) != 1 || !(version & 0x80000000u) ||
        (ocr & 0xc0000000u) != 0xc0000000u || !rca || (csd0 >> 30) != 1 || u32(mmc + 0x20) != 4 || !u32(mmc + 0x24) ||
        u32(mmc + 0x6c) != 512 || u32(mmc + 0x70) != 512 || u32(mmc + 0xdc) || mmc[0xd8] || mmc[0xd9] ||
        !(u32(mmc + 0x50) | u32(mmc + 0x54) | u32(mmc + 0x58) | u32(mmc + 0x5c)) || u64(desc + 0x18) != 512 ||
        u32(desc + 0x20) != 9 || desc[0xb])
        return SWITCH_FW_CARD_STATE;
    uint32_t csd1 = u32(mmc + 0x44), csd2 = u32(mmc + 0x48);
    if (((csd1 >> 16) & 15) != 9)
        return SWITCH_FW_CAPACITY;
    uint64_t sectors = (((uint64_t)(csd1 & 0x3f) << 16) | (csd2 >> 16)) + 1;
    sectors <<= 10;
    uint64_t bytes = sectors << 9;
    if (!sectors || sectors > UINT64_C(0x100000000) || u64(mmc + 0x88) != bytes || u64(mmc + 0x90) != bytes ||
        u64(desc + 0x10) != sectors)
        return SWITCH_FW_CAPACITY;
    for (unsigned i = 0; i < 256; ++i)
        out[i] = 0;
    p32(out, SWITCH_FW_EXPORT_MAGIC);
    p32(out + 4, 1);
    p32(out + 8, 256);
    p32(out + 12, 1);
    p32(out + 0x10, 0x700b0000);
    p32(out + 0x14, 1);
    p32(out + 0x18, 512);
    p32(out + 0x1c, 1);
    p64(out + 0x20, sectors);
    p64(out + 0x28, bytes);
    p32(out + 0x30, ocr);
    p32(out + 0x34, rca);
    p32(out + 0x38, version);
    for (unsigned i = 0; i < 32; ++i)
        out[0x40 + i] = mmc[0x40 + i];
    p64(out + 0x60, mp);
    p64(out + 0x68, found_desc);
    p64(out + 0x70, gd);
    p64(out + 0x78, reloc);
    for (unsigned i = 0; i < 32; ++i)
        out[0x80 + i] = switch_fw_sha256[i];
    p32(out + 0xf0, crc32(out, 0xf0));
    return SWITCH_FW_OK;
}

int switch_fw_capture(const struct switch_fw_reader *r, uint64_t gd, uint8_t output[256])
{
    if (!r || !r->copy || !output)
        return SWITCH_FW_ARGUMENT;
    uint8_t first[256], second[256];
    int error = snapshot(r, gd, first);
    if (error)
        return error;
    error = snapshot(r, gd, second);
    if (error)
        return error;
    if (!same(first, second, 256))
        return SWITCH_FW_CHANGED;
    for (unsigned i = 0; i < 256; ++i)
        output[i] = first[i];
    return SWITCH_FW_OK;
}
