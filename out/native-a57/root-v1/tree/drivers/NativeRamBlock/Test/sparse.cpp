// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SparseDisk.h"
#include <assert.h>
#include <stdlib.h>
#include <vector>
#include <stdio.h>
struct Ops {
    std::vector<uint8_t> buffer;
    long budget = -1;
    uint64_t shortAt = UINT64_MAX;
    unsigned live = 0;
    uint8_t *allocate()
    {
        if (budget == 0) return nullptr;
        if (budget > 0) --budget;
        auto *p = static_cast<uint8_t *>(calloc(1, 4096));
        if (p) ++live;
        return p;
    }
    void release(uint8_t *p)
    {
        assert(live);
        --live;
        free(p);
    }
    uint64_t read(uint64_t at, void *p, uint64_t n)
    {
        if (at >= shortAt) return 0;
        if (n > shortAt - at) n = shortAt - at;
        memcpy(p, buffer.data() + at, n);
        return n;
    }
    uint64_t copy(uint64_t at, const void *p, uint64_t n)
    {
        if (at >= shortAt) return 0;
        if (n > shortAt - at) n = shortAt - at;
        memcpy(buffer.data() + at, p, n);
        return n;
    }
};
static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    memcpy(b.data() + at, &v, 4);
}
static void put64(std::vector<uint8_t> &b, size_t at, uint64_t v)
{
    memcpy(b.data() + at, &v, 8);
}
static void cleanup(RamSparseDisk &d, Ops &op)
{
    for (uint64_t i = 0; i < d.bytes / 4096; ++i)
        if (d.pages[i]) op.release(d.pages[i]);
    free(d.pages);
    d = {};
    assert(!op.live);
}
static RamSparseDisk disk(uint64_t bytes, uint64_t cap)
{
    return {static_cast<uint8_t **>(calloc(bytes / 4096, sizeof(uint8_t *))), bytes, 0, cap};
}
int main()
{
    const uint64_t size = 4096 * 32;
    std::vector<uint8_t> packed(48 + 4096);
    memcpy(packed.data(), "DBRDISK1", 8);
    put32(packed, 8, 1);
    put32(packed, 12, 4096);
    put64(packed, 16, size);
    put64(packed, 24, 1);
    put64(packed, 32, 4096 * 7);
    put64(packed, 40, 4096);
    packed[48 + 9] = 0xa5;
    uint64_t logical = 0;
    assert(ramPackedLayout(packed.data(), packed.size(), &logical) && logical == size);
    for (unsigned which = 0; which < 14; ++which) {
        auto bad = packed;
        if (which == 0) bad[0] = 0;
        if (which == 1) put32(bad, 8, 2);
        if (which == 2) put32(bad, 12, 16384);
        if (which == 3) put64(bad, 16, (1ULL << 40) + 4096);
        if (which == 4) put64(bad, 24, 65537);
        if (which == 5) put64(bad, 32, 1);
        if (which == 6) put64(bad, 40, 0);
        if (which == 7) bad.pop_back();
        if (which == 8) bad.push_back(0);
        if (which == 9) put64(bad, 16, 0);
        if (which == 10) put64(bad, 16, size - 1);
        if (which == 11) put64(bad, 32, UINT64_MAX - 4095);
        if (which == 12) put64(bad, 40, UINT64_MAX - 4095);
        if (which == 13) put64(bad, 32, size);
        logical = 99;
        assert(!ramPackedLayout(bad.data(), bad.size(), &logical) && !logical);
    }
    auto overlap = packed;
    overlap.resize(64 + 8192);
    put64(overlap, 24, 2);
    put64(overlap, 48, 7 * 4096);
    put64(overlap, 56, 4096);
    assert(!ramPackedLayout(overlap.data(), overlap.size(), &logical));
    assert(!ramPackedLayout(nullptr, 32, &logical));
    assert(!ramPackedLayout(packed.data(), packed.size(), nullptr));
    Ops op;
    RamSparseDisk d = disk(size, size);
    assert(ramSparseImport(d, packed.data(), packed.size(), op) && d.resident == 4096);
    std::vector<uint8_t> flat(size);
    flat[7 * 4096 + 9] = 0xa5;
    uint64_t random = 42;
    for (unsigned i = 0; i < 6000; ++i) {
        random = random * 6364136223846793005ULL + 1;
        uint64_t at = random % size;
        uint64_t n = 1 + ((random >> 32) % 65536);
        if (n > size - at) n = size - at;
        op.buffer.assign(n, 0);
        bool write = (i % 3) != 0;
        if (write) {
            for (size_t j = 0; j < n; ++j) op.buffer[j] = static_cast<uint8_t>(i + j);
            assert(ramSparseWriteAtomic(d, at, op.buffer.data(), n, op));
            memcpy(flat.data() + at, op.buffer.data(), n);
        } else {
            bool oom = false;
            assert(ramSparseIO(d, false, at, n, op, oom) == n && !oom);
            assert(!memcmp(flat.data() + at, op.buffer.data(), n));
        }
    }
    op.buffer.assign(size, 0);
    bool oom = false;
    assert(ramSparseIO(d, false, 0, size, op, oom) == size && op.buffer == flat);
    assert(!ramSparseWriteAtomic(d, 0, flat.data(), 65537, op));
    assert(!ramSparseWriteAtomic(d, UINT64_MAX, flat.data(), 1, op));
    assert(!ramSparseWriteAtomic(d, size, flat.data(), 1, op));
    assert(ramSparseIO(d, false, UINT64_MAX, 1, op, oom) == 0);
    cleanup(d, op);
    // Late allocation failure leaves the disk and resident count entirely unchanged.
    d = disk(65536 * 2, 65536 * 2);
    op.buffer.assign(65536, 0x37);
    op.budget = 8;
    assert(!ramSparseWriteAtomic(d, 512, op.buffer.data(), 65536, op) && !d.resident && !op.live);
    for (uint64_t i = 0; i < d.bytes / 4096; ++i) assert(!d.pages[i]);
    op.budget = -1;
    assert(ramSparseWriteAtomic(d, 512, op.buffer.data(), 65536, op) && d.resident == 17 * 4096);
    for (unsigned i = 0; i < 512; ++i) assert(d.pages[0][i] == 0);
    for (unsigned i = 512; i < 4096; ++i) assert(d.pages[16][i] == 0);
    // Existing resident writes remain possible at cap; adding even one page refuses atomically.
    d.residentLimit = d.resident;
    auto resident = d.resident;
    op.buffer.assign(65536, 0x55);
    assert(ramSparseWriteAtomic(d, 0, op.buffer.data(), 65536, op));
    assert(!ramSparseWriteAtomic(d, 65536, op.buffer.data(), 8192, op) && d.resident == resident);
    assert(d.pages[16][0] == 0x37 && !d.pages[17]);
    op.buffer.assign(65536, 0);
    op.shortAt = 5000;
    assert(ramSparseIO(d, false, 0, 65536, op, oom) == 5000 && !oom);
    assert(op.buffer[4999] == 0x55 && op.buffer[5000] == 0);
    cleanup(d, op);
    // Import respects cap and OOM and skips explicit zero seed pages.
    d = disk(size, 0);
    assert(!ramSparseImport(d, packed.data(), packed.size(), op) && !d.resident);
    cleanup(d, op);
    d = disk(size, 4096);
    op.budget = 0;
    assert(!ramSparseImport(d, packed.data(), packed.size(), op) && !d.resident);
    cleanup(d, op);
    d = disk(size, 4096);
    op.budget = -1;
    packed[48 + 9] = 0;
    assert(ramSparseImport(d, packed.data(), packed.size(), op) && !d.resident);
    cleanup(d, op);
    puts("PASS: bounded sparse parser/import, 6000 differential atomic operations, cap, OOM rollback, malformed layout "
         "and short read copies");
}
