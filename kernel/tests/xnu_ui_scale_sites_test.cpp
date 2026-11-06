// bsd/vm/switch_ui_scale_sites.h: the UI scale sites against 21H461's cache text mappings, and the slot each mapping
// maps them by (switch_us_site_for_slot, what switch_shared_cache_text_pager() asks once per slot of a mapping).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include "vm/switch_ui_scale.h"
#include "vm/switch_ui_scale_sites.h"
}

namespace {
// 21H461's subcaches with a patched page: __TEXT is each one's only executable mapping, at file offset 0 (ipsw dyld
// info). Slots another pager takes: slot 0 of .25, .13 and .03 (home grid, settings icons and keyboard, asked before
// any UI scale site) and slots 0 and 1 of .07 (the widget deadlines, asked after: a UI scale site there would hide one).
struct TextMapping {
    const char *name;
    uint8_t uuid[16];
    uint64_t address, size;
    unsigned other_pagers_slots;
};

const TextMapping kMappings[] = {
    {".03",
     {0x1b, 0x9c, 0xcf, 0x0c, 0x7b, 0x68, 0x39, 0x63, 0xaf, 0x64, 0x48, 0x4b, 0xcf, 0xda, 0xc1, 0x4c},
     0x186dac000,
     0x6cb8000,
     1u << 0},
    {".07",
     {0xb8, 0x6b, 0xcb, 0x98, 0x4e, 0x86, 0x33, 0x74, 0xb9, 0x27, 0x98, 0xa7, 0x1e, 0x6b, 0x1e, 0xbd},
     0x1948c8000,
     0x4830000,
     1u << 0 | 1u << 1},
    {".09",
     {0x59, 0xec, 0x4b, 0x71, 0xf0, 0x27, 0x33, 0xd1, 0xae, 0x39, 0x4e, 0x04, 0xbd, 0x70, 0x7b, 0x6d},
     0x199130000,
     0x6b7c000,
     0},
    {".11",
     {0x70, 0x56, 0x7d, 0x89, 0x4c, 0xe6, 0x3e, 0xc6, 0xa4, 0xde, 0xe7, 0xb4, 0xf4, 0xec, 0x60, 0x41},
     0x19fce4000,
     0x6a8c000,
     0},
    {".13",
     {0xc0, 0x45, 0x2d, 0x94, 0xa5, 0xf8, 0x3b, 0xe1, 0xbd, 0xfc, 0xe1, 0x5e, 0x71, 0x4a, 0x84, 0xf5},
     0x1a67c8000,
     0x6c60000,
     1u << 0},
    {".25",
     {0xf1, 0x6d, 0xce, 0xf8, 0xc4, 0x46, 0x38, 0x05, 0x91, 0x63, 0xc0, 0x13, 0x8e, 0x4d, 0xca, 0xd1},
     0x1ce094000,
     0x6adc000,
     1u << 0},
    {".31",
     {0x64, 0xa2, 0xf3, 0x51, 0xbe, 0x93, 0x39, 0xa6, 0x93, 0x72, 0xe8, 0x56, 0xb6, 0x64, 0x9c, 0xfc},
     0x1e221c000,
     0x40ac000,
     0},
};

constexpr uint64_t kChunk = 0x1000;

const TextMapping *MappingOf(const switch_us_site &site)
{
    for (const TextMapping &mapping : kMappings) {
        if (std::memcmp(mapping.uuid, site.uuid, sizeof(site.uuid)) == 0) {
            return &mapping;
        }
    }
    return nullptr;
}

std::string NameOf(const switch_us_site &site)
{
    return std::string(site.name) + " " + site.applied;
}

const switch_us_site *Named(const std::string &name)
{
    for (const switch_us_site &site : switch_us_sites) {
        if (NameOf(site) == name) {
            return &site;
        }
    }
    ADD_FAILURE() << "no site " << name;
    return nullptr;
}

uint32_t Word(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

unsigned TextSlots(const char *vm_shared_region)
{
    const std::string source = switch_ios::test::SourceText(vm_shared_region);
    const std::string define = "#define SWITCH_TEXT_SLOTS ";
    const size_t at = source.find(define);
    if (at == std::string::npos) {
        ADD_FAILURE() << "no SWITCH_TEXT_SLOTS in " << vm_shared_region;
        return 0;
    }
    return static_cast<unsigned>(std::stoul(source.substr(at + define.size())));
}
}  // namespace

TEST(XnuUiScaleSites, EveryEditIsInItsSitesChunkOfItsSubcache)
{
    for (const switch_us_site &site : switch_us_sites) {
        SCOPED_TRACE(NameOf(site));
        const TextMapping *mapping = MappingOf(site);
        ASSERT_NE(mapping, nullptr);
        EXPECT_EQ(site.page_vmaddr - site.page_fileoff, mapping->address);
        EXPECT_EQ(site.page_vmaddr % SWITCH_US_PAGE_SIZE, 0u);
        EXPECT_EQ(site.chunk_offset % kChunk, 0u);
        EXPECT_LT(site.chunk_offset, SWITCH_US_PAGE_SIZE);
        ASSERT_GE(site.edits, 1u);
        ASSERT_LE(site.edits, sizeof(site.edit) / sizeof(site.edit[0]));
        const uint64_t chunk = site.page_fileoff + site.chunk_offset;
        for (unsigned i = 0; i < site.edits; i++) {
            const switch_us_edit &edit = site.edit[i];
            EXPECT_GE(edit.fileoff, chunk) << i;
            EXPECT_LE(edit.fileoff + edit.length, chunk + kChunk) << i;
            EXPECT_LE(edit.length, sizeof(edit.stock)) << i;
            EXPECT_LE(edit.at + edit.size, edit.length) << i;
            if (edit.kind == SWITCH_US_FIXED) {
                EXPECT_LE(edit.size, sizeof(edit.patched)) << i;
            }
        }
    }
}

TEST(XnuUiScaleSites, EachSiteIsTheOneSiteOfItsSlotInItsMapping)
{
    const unsigned slots = TextSlots("../xnu-10063.141.1/osfmk/vm/vm_shared_region.c");
    EXPECT_EQ(TextSlots("../xnu-12377.121.6/osfmk/vm/vm_shared_region.c"), slots);
    std::vector<unsigned> found(SWITCH_US_SITES, 0);
    for (const TextMapping &mapping : kMappings) {
        for (unsigned slot = 0; slot < slots; slot++) {
            SCOPED_TRACE(std::string(mapping.name) + " slot " + std::to_string(slot));
            const switch_us_site *site = switch_us_site_for_slot(slot, mapping.address, mapping.size, 0);
            if (site == nullptr) {
                continue;
            }
            EXPECT_EQ(MappingOf(*site), &mapping) << NameOf(*site);
            EXPECT_EQ(site->slot, slot) << NameOf(*site);
            EXPECT_EQ(mapping.other_pagers_slots & 1u << slot, 0u)
                << NameOf(*site) << " shares a slot with another pager";
            found[static_cast<size_t>(site - switch_us_sites)]++;
        }
    }
    for (size_t i = 0; i < SWITCH_US_SITES; i++) {
        EXPECT_EQ(found[i], 1u) << NameOf(switch_us_sites[i]) << " (slot " << switch_us_sites[i].slot << ")";
    }
    // a mapping that starts after a site's page, ends inside it or maps another file offset holds none of it
    for (const switch_us_site &site : switch_us_sites) {
        SCOPED_TRACE(NameOf(site));
        const uint64_t address = MappingOf(site)->address, size = MappingOf(site)->size;
        EXPECT_TRUE(switch_us_site_in_mapping(&site, address, size, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, site.page_vmaddr + SWITCH_US_PAGE_SIZE, size, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, address, site.page_vmaddr - address + 0x1000, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, address, size, SWITCH_US_PAGE_SIZE));
    }
}

TEST(XnuUiScaleSites, BothUIKitCoreBatteryFillsAreOneEditOfTheSameCode)
{
    // SpringBoard's status bar and Control Center draw _UIBatteryViews, UIKit's own status bar a
    // _UIStatusBarBatteryView: two copies of _fillLayerFrame:cornerRadius:, the same but for one bl's displacement
    const switch_us_site *battery = Named("UIKitCore _UIBatteryView fill");
    const switch_us_site *status_bar = Named("UIKitCore _UIStatusBarBatteryView fill");
    ASSERT_NE(battery, nullptr);
    ASSERT_NE(status_bar, nullptr);
    ASSERT_EQ(battery->edits, 2u);
    ASSERT_EQ(status_bar->edits, 2u);
    EXPECT_EQ(battery->edit[1].fileoff - battery->edit[0].fileoff,
              status_bar->edit[1].fileoff - status_bar->edit[0].fileoff);
    for (unsigned i = 0; i < 2; i++) {
        SCOPED_TRACE(i);
        const switch_us_edit &a = battery->edit[i], &b = status_bar->edit[i];
        EXPECT_EQ(a.kind, b.kind);
        EXPECT_EQ(a.length, b.length);
        EXPECT_EQ(a.at, b.at);
        EXPECT_EQ(a.size, b.size);
        EXPECT_EQ(std::memcmp(a.patched, b.patched, sizeof(a.patched)), 0);
    }
    EXPECT_EQ(std::memcmp(battery->edit[0].stock, status_bar->edit[0].stock, battery->edit[0].length), 0);
    EXPECT_EQ(std::memcmp(battery->edit[1].stock, status_bar->edit[1].stock, 16), 0);
    EXPECT_EQ(Word(battery->edit[1].stock + 16) >> 26, 0x25u);  // bl CGRectGetWidth
    EXPECT_EQ(Word(status_bar->edit[1].stock + 16) >> 26, 0x25u);
}

TEST(XnuUiScaleSites, CoverSheetKitRowIsTheIPadWithoutAHomeButton)
{
    // +[CSDeviceDetermination categoryFromScreenSize]: bl BSSizeEqualToSize; tbz w0, #0, (mov w0, #28); mov w0, #24
    const switch_us_site *row = Named("CoverSheetKit lock screen layout row");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->edits, 1u);
    const switch_us_edit &edit = row->edit[0];
    ASSERT_EQ(edit.kind, SWITCH_US_FIXED);
    ASSERT_EQ(edit.length, 12u);
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);
    const uint32_t tbz = Word(edit.stock + 4);
    EXPECT_EQ(tbz & 0xfff8001fu, 0x36000000u);
    const int32_t imm14 = static_cast<int32_t>((tbz >> 5) & 0x3fff);
    const int64_t tbz_address = static_cast<int64_t>(row->page_vmaddr + (edit.fileoff - row->page_fileoff) + 4);
    EXPECT_EQ(tbz_address + (imm14 >= 0x2000 ? imm14 - 0x4000 : imm14) * 4, 0x1ac8c2410);
    EXPECT_EQ(Word(edit.stock + 8), switch_a64_movz_w(0, 24));
    EXPECT_EQ(edit.at, 4u);
    EXPECT_EQ(edit.size, 4u);
    EXPECT_EQ(Word(edit.patched), 0xd503201fu);  // nop
}

TEST(XnuUiScaleSites, CoreUiRowBytes)
{
    // both CoreUI sites: ... ucvtf d0, x24; fmul d0, dW, d0; fcvtzu x3, d0 -> mul x3, x0, x24; nop; nop
    const std::vector<uint32_t> row_bytes = {0x9b187c03u, 0xd503201fu, 0xd503201fu};
    for (const char *name : {"CoreUI PDF vector image row bytes", "CoreUI SVG vector image row bytes"}) {
        SCOPED_TRACE(name);
        const switch_us_site *site = Named(name);
        ASSERT_NE(site, nullptr);
        const switch_us_edit &edit = site->edit[0];
        ASSERT_EQ(edit.size, 12u);
        EXPECT_EQ((std::vector<uint32_t>{Word(edit.patched), Word(edit.patched + 4), Word(edit.patched + 8)}),
                  row_bytes);
    }
}

TEST(XnuUiScaleSitesSource, BothTreesAndNoPagerAtScaleOne)
{
    const bool same = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/vm/switch_ui_scale_sites.h") ==
                      switch_ios::test::SourceText("../xnu-10063.141.1/bsd/vm/switch_ui_scale_sites.h");
    EXPECT_TRUE(same) << "the two trees' switch_ui_scale_sites.h differ";
    for (const char *tree : {"../xnu-10063.141.1/bsd/vm/vm_unix.c", "../xnu-12377.121.6/bsd/vm/vm_unix.c"}) {
        SCOPED_TRACE(tree);
        const std::string vm_unix = switch_ios::test::SourceText(tree);
        const std::string pager =
            switch_ios::test::Body(vm_unix, "switch_ui_scale_pager(const struct switch_us_site *site");
        const size_t unit = pager.find("if (switch_ui_scale_is_unit(switch_ui_scale())) {");
        ASSERT_NE(unit, std::string::npos);
        EXPECT_LT(unit, pager.find("vn_rdwr("));
        EXPECT_LT(unit, pager.find("apple_protect_pager_setup("));
        // a site with no bytes at the boot's scale (the battery fills at 1.6) maps no pager either
        const size_t no_bytes = pager.find("!switch_ui_scale_edit_bytes(site->edit[i].kind, switch_ui_scale()");
        ASSERT_NE(no_bytes, std::string::npos);
        EXPECT_LT(no_bytes, pager.find("apple_protect_pager_setup("));
        // the dispatch asks the table which site a slot maps
        const std::string dispatch = switch_ios::test::Body(vm_unix, "\nswitch_shared_cache_text_pager(");
        EXPECT_NE(dispatch.find("switch_us_site_for_slot(slot, unslid_address, size, file_offset)"), std::string::npos);
    }
}
