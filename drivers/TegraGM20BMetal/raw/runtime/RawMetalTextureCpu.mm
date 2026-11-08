// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// A shared mipmapped texture's CPU writes reach its chain's states (raw::Texture::Chain, _mipL0/_mipLevels: the submit lock's) at
// the next submission, before it encodes: Metal leaves a CPU access racing GPU work that uses the texture undefined, so every
// command buffer committed before the write has completed and every later one encodes after the fold.
static std::mutex gCpuLevelsLock;
static std::vector<RMTexture *> gCpuLevelsPending;
static std::atomic<bool> gCpuLevelsAny{false};

void noteCpuLevels(RMTexture *t, uint32_t bits)
{
    if (t->_cpuLevels.fetch_or(bits, std::memory_order_acq_rel))
        return;
    std::lock_guard<std::mutex> l(gCpuLevelsLock);
    gCpuLevelsPending.push_back(t);
    gCpuLevelsAny.store(true, std::memory_order_release);
}

// The submit lock held: the pending CPU writes into the chains' states. A later level the CPU wrote into the chain is the
// chain's own (rawLevels: m2v copies it in, never generates it), newer than its mirror, and makes the chain's levels the ones
// held when nothing generated them before (m2v held none: cpuLevel wrote into the chain only then).
void foldCpuLevels()
{
    if (!gCpuLevelsAny.load(std::memory_order_acquire))
        return;
    std::vector<RMTexture *> list;
    {
        std::lock_guard<std::mutex> l(gCpuLevelsLock);
        list.swap(gCpuLevelsPending);
        gCpuLevelsAny.store(false, std::memory_order_relaxed);
    }
    for (RMTexture *t : list) {
        const uint32_t m = t->_cpuLevels.exchange(0, std::memory_order_acq_rel);
        raw::Texture::Chain &c = *t->_raw.chain;
        if (m & 1) {
            c.l0Fresh = false;
            t->_mipL0 = false;
        }
        if (const uint16_t own = uint16_t(m & 0xfffe)) {
            c.levelsFresh = true;
            c.rawLevels |= own;
            c.mirrorLevels &= uint16_t(~own);
            t->_mipLevels = false;
        }
        c.mirrorLevels |= uint16_t(m >> 16);
    }
}

static void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(from) & ~uintptr_t(63); a < uintptr_t(from) + bytes; a += 64)
        if (invalidate)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)from;
    (void)bytes;
    (void)invalidate;
#endif
}

// replaceRegion/getBytes of later level `level` of a shared mipmapped texture, where the level is held (the states as the work
// committed before left them, and the CPU writes since): m2v's texture of every level after m2v generated them (a write also goes
// to the level's mirror, whole, so the raw path takes it in after generating: Chain::mirrorLevels); the mirror after m2v wrote
// the level (and m2v's texture); else the chain, in the CPU's view the device's self-test chose.
// A block-linear texture's CPU copies (replaceRegion, getBytes of level 0) through TegraGM20BLayout.h: the block rows they
// reach are invalidated before a read of what the GPU wrote, cleaned after a write
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    tgm_layout l;
    if (!r.size.width || !r.size.height || !tgm_layout_tiled(t->_raw.width, t->_raw.height, t->_raw.bpp, &l))
        return;
    const uint64_t blockRow = uint64_t(l.row_bytes) * (8u << l.y_log2);
    const uint64_t from = r.origin.y / (8u << l.y_log2) * blockRow,
                   to = std::min<uint64_t>((r.origin.y + r.size.height + (8u << l.y_log2) - 1) / (8u << l.y_log2) *
                                               blockRow,
                                           uint64_t(l.row_bytes) * l.rows);
    if (write) {
        tgm_layout_write(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                         uint32_t(r.size.height), bytes, row);
        cacheLines(t->_raw.cpu + from, to - from, false);
        return;
    }
    if (t->_gpuDirty.exchange(false))
        cacheLines(t->_raw.cpu, l.row_bytes * uint64_t(l.rows), true);
    tgm_layout_read(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                    uint32_t(r.size.height), bytes, row);
}

void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture::Chain &c = *t->_raw.chain;
    if (level >= c.layout.levels)
        return;
    const raw::fx::MipLevel &lv = c.layout.level[level];
    if (r.origin.x + r.size.width > lv.width || r.origin.y + r.size.height > lv.height)
        return;
    const uint32_t bit = 1u << level, pending = t->_cpuLevels.load(std::memory_order_acquire);
    id<MTLTexture> all = t->_mipTwin;
    const bool m2v = !(pending & bit) && !(pending >> 16 & bit) && !c.levelsFresh && all;
    RMTexture *mirror = nil;
    if (m2v || (pending >> 16 & bit) || (!(pending & bit) && (c.mirrorLevels & bit)))
        mirror = [t rmMirror:level];
    if (m2v || mirror) {
        if (write && all)
            [all replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
        if (!mirror) {
            if (write)
                once(@"a CPU write to a level of a mipmapped texture m2v holds has no mirror: the raw path's copy of "
                     @"the "
                     @"level stays generated from level 0");
            else
                [all getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
            return;
        }
        const raw::Texture &m = mirror->_raw;
        const uint64_t span = uint64_t(m.pitch) * lv.height;
        if (m2v && write) {
            [all getBytes:m.cpu
                  bytesPerRow:m.pitch
                bytesPerImage:0
                   fromRegion:MTLRegionMake2D(0, 0, lv.width, lv.height)
                  mipmapLevel:level
                        slice:0];
        } else if (m2v) {
            [all getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
            return;
        } else {
            if (!write)
                cacheLines(m.cpu, span, true);
            const size_t line = r.size.width * m.bpp;
            for (NSUInteger y = 0; y < r.size.height; y++) {
                uint8_t *at = m.cpu + (r.origin.y + y) * m.pitch + r.origin.x * m.bpp;
                if (write)
                    memcpy(at, bytes + y * row, line);
                else
                    memcpy(bytes + y * row, at, line);
            }
        }
        if (write) {
            cacheLines(m.cpu, span, false);
            noteCpuLevels(t, bit << 16);
        }
        return;
    }
    uint8_t *chain = c.span.cpu;
    const uint64_t at = lv.offset, span = uint64_t(lv.rowBytes) * lv.rows;
    if (write) {
        raw::fx::writeLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                            uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
        cacheLines(chain + at, span, false);
        noteCpuLevels(t, bit);
    } else {
        cacheLines(chain + at, span, true);
        raw::fx::readLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                           uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
    }
}
