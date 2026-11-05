// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 MC SMMU registers under one lock (TegraSMMU.h). Short register sequences only: no allocation, no waits.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include "TegraSMMU.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {
constexpr vm_offset_t kMcPA = 0x70019000;
enum : uint32_t {
    PTB_ASID = 0x1c,
    PTB_DATA = 0x20,
    TLB_FLUSH = 0x30,
    PTC_FLUSH = 0x34,
    ASID_ENABLE = 1u << 31,
    TLB_FLUSH_ASID_MATCH = 1u << 31,
};
volatile uint8_t *gMc;
IOSimpleLock *gLock;

inline uint32_t r32(uint32_t o)
{
    return *(volatile uint32_t *)(gMc + o);
}
inline void w32(uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(gMc + o) = v;
}
// The registers and the lock, made by the first caller (thread context).
bool ready()
{
    if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE)) {
        IOSimpleLock *fresh = IOSimpleLockAlloc(), *expected = nullptr;
        if (!fresh) return false;
        if (!__atomic_compare_exchange_n(&gLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOSimpleLockFree(fresh);
    }
    if (!__atomic_load_n(&gMc, __ATOMIC_ACQUIRE)) {
        volatile uint8_t *mc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000), *expected = nullptr;
        if (!mc) return false;
        __atomic_compare_exchange_n(&gMc, &expected, mc, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return true;
}
// Lock held.
void flushLocked(uint32_t asid)
{
    w32(PTC_FLUSH, 0);  // all
    (void)r32(PTB_ASID);
    w32(TLB_FLUSH, TLB_FLUSH_ASID_MATCH | (asid << 24));  // this ASID, every address
    (void)r32(PTB_ASID);
}
}  // namespace

extern "C" int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                                 uint32_t translationBits, char *line, size_t size)
{
    if (!ready()) {
        snprintf(line, size, "cannot map the memory controller");
        return -1;
    }
    IOSimpleLockLock(gLock);
    int ret = -1;
    const uint32_t was = r32(asidReg);
    if ((was & ASID_ENABLE) && (was & 0x7f) != asid) {
        snprintf(line, size, "the swgroup is already translated by ASID %u (MC %#x = %#x)", was & 0x7f, asidReg, was);
        goto out;
    }
    w32(PTB_ASID, asid);
    w32(PTB_DATA, ptb);
    (void)r32(PTB_ASID);
    w32(PTB_ASID, asid);
    if (r32(PTB_DATA) != ptb) {
        snprintf(line, size, "ASID %u directory did not take (PTB_DATA %#x, wrote %#x)", asid, r32(PTB_DATA), ptb);
        goto out;
    }
    flushLocked(asid);
    w32(translationReg, r32(translationReg) | translationBits);
    w32(asidReg, ASID_ENABLE | asid);
    (void)r32(asidReg);
    if (r32(asidReg) != (ASID_ENABLE | asid)) {
        snprintf(line, size, "swgroup ASID did not take (MC %#x = %#x)", asidReg, r32(asidReg));
        goto out;
    }
    flushLocked(asid);
    ret = 0;
out:
    IOSimpleLockUnlock(gLock);
    return ret;
}

extern "C" void tegra_smmu_detach(uint32_t asid, uint32_t asidReg)
{
    if (!ready()) return;
    IOSimpleLockLock(gLock);
    w32(asidReg, 0);
    (void)r32(asidReg);
    w32(PTB_ASID, asid);
    w32(PTB_DATA, 0);
    flushLocked(asid);
    IOSimpleLockUnlock(gLock);
}

extern "C" void tegra_smmu_flush(uint32_t asid)
{
    if (!ready()) return;
    IOSimpleLockLock(gLock);
    flushLocked(asid);
    IOSimpleLockUnlock(gLock);
}

extern "C" uint32_t tegra_smmu_read(uint32_t offset)
{
    return ready() ? r32(offset) : 0;
}
