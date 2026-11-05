// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra X1+ Security Engine (SE1 at 0x70012000, SE2 at 0x70412000) as the AppleKeyStore stand-in's hardware root: AES-CMAC of
// fixed labels under a keyslot the boot ROM loaded and nobody can read back (SE1 slot 14, the per-console Secure Boot Key on
// Mariko; hekate derives the HOS device key from it). This file holds the boot probe that shows whether that slot is usable from
// XNU; the device key moves here once a boot has shown it is.
//
// Registers and sequences: NVIDIA Tegra X1 TRM (Security Engine); hekate bdk/sec/se_t210.h and se.c (CTCaer, naehrwert;
// GPL-2.0); switchroot ATF plat/nvidia/tegra/soc/t210/drivers/se (NVIDIA; BSD-3-Clause); L4T drivers/crypto/tegra-se.c
// (NVIDIA; GPL-2.0). Written from the register facts in those sources, no code copied.
//
// What reaches XNU:
//   - hekate's _config_se_brom rewrites slot 14 from the fuses only while FUSE_PRIVATE_KEY0 still answers (once the boot ROM has
//     disabled the private key it reads all ones), and its L4T launch (launch_l4t, hw_deinit, _l4t_late_hw_config) never touches
//     the SE. _se_lock, which makes every slot secure-only, runs only on the HOS path.
//   - switchroot BL31 tegra_se_init only regenerates the SRK of SE1 and SE2 (SE_CONFIG DST=SRK: no keyslot, no SE_SECURITY). Its
//     tegra_se_lock ORs in SE_SECURE = 0, a no-op, and runs only on T210's software context save.
//   - SC7 (BL31 tegra_se_suspend): atomic context save, then the SE clock off and the SE in reset with every other DMA master; the
//     boot ROM restores the keyslots on the way out. Anything that uses the SE after a resume must check clock and reset again.
//
// The probe never writes a keyslot, its IVs or the access registers: one AES-128 ECB block per operation (XOR bypass, so neither
// the original nor the updated IV is used or changed), chained into CMAC in software (AKSCmac.h), only over the fixed label below,
// so EL1 gains no oracle on the key. It logs 4 bytes of SHA-256 of each MAC, never key material. Each operation is a bounded
// register handshake of a few microseconds, started only after the security, per-key and access registers allow it and the
// engine has taken our configuration; one that then does not finish panics. Anything else is logged and ends the probe.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
#include <string.h>
#include "AKSCmac.h"
#include "../TegraSE/TegraSE.h"

namespace {
constexpr uint64_t kCarPA = 0x60006000, kAhbArbPA = 0x6000C000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000;
constexpr uint32_t kCarRstDevicesH = 0x008, kCarClkOutEnbH = 0x014, kCarRstDevicesV = 0x358, kCarClkOutEnbV = 0x360,
                   kCarClkSourceSe = 0x42c;
constexpr uint32_t kFuseClockBit = 1u << 7;  // CLK_H_FUSE
// CLK_V_SE, shared by SE1 and SE2 (ATF tegra_se_enable_clocks; the DTB gives both the same "se" clock).
constexpr uint32_t kSeClockBit = 1u << 31;
// FUSE 0x7000F800 + 0x28: bit 0 private key disabled, bit 4 TZ sticky.
constexpr uint32_t kFusePrivateKeyDisable = 0x828;
// SE DMA over AHB is MC client group PPCS (ATF tegra_se_suspend disables exactly that ASID before its own SE operations).
constexpr uint32_t kMcSmmuConfig = 0x010, kMcSmmuPpcsAsid = 0x270, kSmmuEnable = 1u, kAsidEnable = 1u << 31;
// AHB_ARBITRATION_AHB_MEM_WRQUE_MST_ID: writes still queued, one bit per AHB master.
constexpr uint32_t kAhbMemWrqueMstId = 0x0fc;

// SE registers (hekate se_t210.h).
enum : uint32_t {
    kSecurity = 0x000,
    kOperation = 0x008,
    kIntStatus = 0x010,
    kConfig = 0x014,
    kInList = 0x018,
    kOutList = 0x024,
    kPerKey = 0x280,
    kKeyAccess = 0x284,
    kCryptoConfig = 0x304,
    kLastBlock = 0x318,
    kStatus = 0x800,
    kErrStatus = 0x804,
};
constexpr uint32_t kSecuritySoftNonsecure = 1u << 16, kSecurityEngineDisabled = 1u << 1;
// KEYTABLE_ACCESS: a set bit allows (hekate se_key_acc_ctrl writes the inverted disable flags).
constexpr uint32_t kKeyUse = 1u << 6;
constexpr uint32_t kIntOpDone = 1u << 4, kIntError = 1u << 16;
constexpr uint32_t kStatusStateMask = 3, kStatusMemBusy = 1u << 2;
constexpr uint32_t kOpStart = 1;
constexpr uint32_t kConfigEcbEncrypt = 1u << 12;  // ENC_ALG AES_ENC, ENC_MODE KEY128, DEC_ALG NOP, DST MEMORY
// CORE_SEL encrypt; XOR bypass (ECB), input from memory, original IV (unused without XOR), AHB memory interface, no hash.
constexpr uint32_t cryptoEcbEncrypt(uint32_t slot)
{
    return slot << 24 | 1u << 8;
}

constexpr uint32_t kOpTimeoutMs = 10;  // one block takes microseconds

// One page below 4 GiB (the list registers hold 32-bit addresses): the two one-entry lists ({last index 0, address, size}),
// the input block and the output block in a cache line of its own.
constexpr uint32_t kDmaInList = 0x00, kDmaOutList = 0x10, kDmaIn = 0x40, kDmaOut = 0x80, kDmaUsed = 0xc0;

struct Engine {
    const char *name;
    uint64_t physical;
    uint32_t ahbMaster;  // bit in MEM_WRQUE_MST_ID: SE1 14, SE2 13 (the DTB's nvidia,ahb-master-id)
    volatile uint32_t *regs;
    uint32_t r(uint32_t offset) const { return regs[offset / 4]; }
    void w(uint32_t offset, uint32_t value) const { regs[offset / 4] = value; }
};

struct Dma {
    IOBufferMemoryDescriptor *memory;
    uint8_t *va;
    uint32_t pa;
};

struct Block {
    const Engine *se;
    Dma *dma;
    volatile uint32_t *ahb;
    uint32_t slot;
    uint32_t error;       // ERR_STATUS (or INT_STATUS) of a failed operation
    const char *refusal;  // why an operation was not started
};

volatile uint32_t *mapRegisters(uint64_t physical, IOMemoryMap **map)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, 0x1000);
    *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    return *map ? reinterpret_cast<volatile uint32_t *>((*map)->getVirtualAddress()) : nullptr;
}

void cleanInvalidate(const void *va, size_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + size; a < end; a += line)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

bool allocateDma(Dma *dma)
{
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous |
                                 (IOOptionBits)kIOMemoryMapperNone;
    dma->memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, PAGE_SIZE, 0xfffff000ULL);
    if (!dma->memory || dma->memory->prepare() != kIOReturnSuccess) {
        OSSafeReleaseNULL(dma->memory);
        return false;
    }
    IOByteCount length = 0;
    const uint64_t pa = dma->memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    dma->va = static_cast<uint8_t *>(dma->memory->getBytesNoCopy());
    if (!pa || !dma->va || length < kDmaUsed || pa + kDmaUsed > (1ULL << 32)) {
        dma->memory->complete();
        OSSafeReleaseNULL(dma->memory);
        return false;
    }
    dma->pa = (uint32_t)pa;
    memset(dma->va, 0, kDmaUsed);
    cleanInvalidate(dma->va, kDmaUsed);
    return true;
}

void releaseDma(Dma *dma)
{
    if (!dma->memory) return;
    memset(dma->va, 0, kDmaUsed);
    cleanInvalidate(dma->va, kDmaUsed);
    dma->memory->complete();
    OSSafeReleaseNULL(dma->memory);
}

// Waits for `done` within the operation's deadline; a handshake that never finishes is the one-line panic.
template <typename Done> void waitFor(const Block &b, uint64_t deadline, const char *what, Done done)
{
    while (!done()) {
        if (mach_absolute_time() > deadline && !done()) {
            panic("AppleKeyStore: Security Engine %s slot %u: %s not within %u ms (INT_STATUS %#x STATUS %#x "
                  "ERR_STATUS %#x)",
                  b.se->name, b.slot, what, kOpTimeoutMs, b.se->r(kIntStatus), b.se->r(kStatus), b.se->r(kErrStatus));
        }
    }
}

// aks::BlockEncrypt: one AES-128 ECB block under the slot, polled (the one-shot boot handshake).
bool ecbBlock(void *context, const uint8_t in[16], uint8_t out[16])
{
    Block &b = *static_cast<Block *>(context);
    const Engine &se = *b.se;
    const Dma &dma = *b.dma;
    if (se.r(kStatus) & (kStatusStateMask | kStatusMemBusy)) {
        b.refusal = "engine busy";
        return false;
    }
    uint32_t *list = reinterpret_cast<uint32_t *>(dma.va);
    const uint32_t lists[8] = {0, dma.pa + kDmaIn, 16, 0, 0, dma.pa + kDmaOut, 16, 0};
    memcpy(list, lists, sizeof lists);
    memcpy(dma.va + kDmaIn, in, 16);
    memset(dma.va + kDmaOut, 0, 16);
    cleanInvalidate(dma.va, kDmaUsed);

    se.w(kConfig, kConfigEcbEncrypt);
    se.w(kCryptoConfig, cryptoEcbEncrypt(b.slot));
    se.w(kLastBlock, 0);  // one block
    se.w(kInList, dma.pa + kDmaInList);
    se.w(kOutList, dma.pa + kDmaOutList);
    se.w(kErrStatus, se.r(kErrStatus));
    se.w(kIntStatus, se.r(kIntStatus));
    // An engine that drops non-secure writes would never finish the operation: check it took ours before starting.
    if (se.r(kConfig) != kConfigEcbEncrypt || se.r(kCryptoConfig) != cryptoEcbEncrypt(b.slot) ||
        se.r(kInList) != dma.pa + kDmaInList || se.r(kOutList) != dma.pa + kDmaOutList) {
        b.refusal = "register writes ignored";
        return false;
    }
    if (se.r(kErrStatus)) {
        b.error = se.r(kErrStatus);
        b.refusal = "error before start";
        return false;
    }

    __asm__ volatile("dsb sy" ::: "memory");
    se.w(kOperation, kOpStart);
    uint64_t deadline;
    clock_interval_to_deadline(kOpTimeoutMs, kMillisecondScale, &deadline);
    waitFor(b, deadline, "operation end",
            [&] { return (se.r(kIntStatus) & (kIntOpDone | kIntError)) || se.r(kErrStatus); });
    // T210B01: the output is in memory once the engine is idle, its memory interface quiet and the AHB write queue drained (L4T
    // tegra_se_start_operation ahb_ack, hekate _se_op_wait).
    waitFor(b, deadline, "memory drain", [&] {
        return !(se.r(kStatus) & (kStatusStateMask | kStatusMemBusy)) && !(b.ahb[kAhbMemWrqueMstId / 4] & se.ahbMaster);
    });
    const uint32_t interrupt = se.r(kIntStatus), error = se.r(kErrStatus);
    se.w(kIntStatus, interrupt);
    se.w(kErrStatus, error);
    bool ok = !error && !(interrupt & kIntError) && (interrupt & kIntOpDone);
    if (ok) {
        cleanInvalidate(dma.va + kDmaOut, 16);
        memcpy(out, dma.va + kDmaOut, 16);
    } else {
        b.error = error ? error : interrupt;
    }
    memset(dma.va + kDmaIn, 0, 16);
    memset(dma.va + kDmaOut, 0, 16);
    cleanInvalidate(dma.va, kDmaUsed);
    return ok;
}

bool softwareEncrypt(void *context, const uint8_t in[16], uint8_t out[16])
{
    aes_encrypt(in, out, static_cast<aes_encrypt_ctx *>(context));
    return true;
}

// The probe's domain label: not the device key's, so the logged fingerprints say nothing about that key.
const char kProbeLabel[] = "Switch SE probe v1";

struct Result {
    const char *refusal = nullptr;
    uint32_t error = 0;
    bool ran = false, stable = false;
    uint8_t mac[16] = {};
};

void probeSlot(const Engine &se, Dma &dma, volatile uint32_t *ahb, uint32_t slot, Result &result)
{
    const uint32_t security = se.r(kSecurity), perKey = se.r(kPerKey), access = se.r(kKeyAccess + 4 * slot);
    result.refusal = !(security & kSecuritySoftNonsecure)   ? "engine secure-only"
                     : (security & kSecurityEngineDisabled) ? "engine disabled"
                     : !(perKey & (1u << slot))             ? "slot secure-only"
                     : !(access & kKeyUse)                  ? "key use disabled"
                                                            : nullptr;
    if (result.refusal) return;
    Block b{&se, &dma, ahb, slot, 0, nullptr};
    uint8_t again[16];
    const uint8_t *label = reinterpret_cast<const uint8_t *>(kProbeLabel);
    if (!aks::cmac(ecbBlock, &b, label, sizeof kProbeLabel - 1, result.mac) ||
        !aks::cmac(ecbBlock, &b, label, sizeof kProbeLabel - 1, again)) {
        result.refusal = b.refusal ? b.refusal : "operation error";
        result.error = b.error;
        aks::wipe(result.mac, sizeof result.mac);
        return;
    }
    result.ran = true;
    result.stable = !memcmp(result.mac, again, 16);
    aks::wipe(again, sizeof again);
}

uint32_t fingerprint(const uint8_t mac[16])
{
    SHA256_CTX c;
    uint8_t digest[32];
    SHA256_Init(&c);
    SHA256_Update(&c, mac, 16);
    SHA256_Final(digest, &c);
    return (uint32_t)digest[0] << 24 | (uint32_t)digest[1] << 16 | (uint32_t)digest[2] << 8 | digest[3];
}

void describe(char *line, size_t size, const char *name, const Result &r, const uint8_t zeroKey[16])
{
    if (!r.ran) {
        snprintf(line, size, "%s not run: %s %#x", name, r.refusal ? r.refusal : "engine not readable", r.error);
        return;
    }
    static const uint8_t zero[16] = {};
    snprintf(line, size, "%s fp %08x stable %d nonzero %d zero-key %d", name, fingerprint(r.mac), r.stable,
             memcmp(r.mac, zero, 16) != 0, !memcmp(r.mac, zeroKey, 16));
}
}  // namespace

// From native_aks_device_key_start, once per boot (sheng-aks=1 with key wrapping on); changes nothing the device key uses.
extern "C" void native_aks_se_probe(void)
{
    static bool probed;
    if (probed) return;
    probed = true;

    IOMemoryMap *carMap = nullptr, *ahbMap = nullptr, *fuseMap = nullptr, *mcMap = nullptr, *se1Map = nullptr,
                *se2Map = nullptr;
    volatile uint32_t *car = mapRegisters(kCarPA, &carMap), *ahb = mapRegisters(kAhbArbPA, &ahbMap),
                      *fuse = mapRegisters(kFusePagePA, &fuseMap), *mc = mapRegisters(kMcPA, &mcMap);
    Engine se1{"SE1", 0x70012000, 1u << 14, nullptr}, se2{"SE2", 0x70412000, 1u << 13, nullptr};
    se1.regs = mapRegisters(se1.physical, &se1Map);
    se2.regs = mapRegisters(se2.physical, &se2Map);
    Dma dma{};
    Result r14, r15, r2;
    if (!car || !ahb || !fuse || !mc || !se1.regs || !se2.regs) {
        IOLog("AppleKeyStore: SE probe: cannot map CAR/AHB/FUSE/MC/SE\n");
        goto out;
    }
    {
        const uint32_t clocks = car[kCarClkOutEnbV / 4], resets = car[kCarRstDevicesV / 4];
        const bool fuseOn = (car[kCarClkOutEnbH / 4] & kFuseClockBit) && !(car[kCarRstDevicesH / 4] & kFuseClockBit);
        const uint32_t privateKey = fuseOn ? fuse[kFusePrivateKeyDisable / 4] : ~0u;
        const uint32_t smmu = mc[kMcSmmuConfig / 4], ppcs = mc[kMcSmmuPpcsAsid / 4];
        // A clock-gated or reset APB block can hang the bus: no SE register is read unless the clock runs and reset is off.
        if (!(clocks & kSeClockBit) || (resets & kSeClockBit)) {
            IOLog("AppleKeyStore: SE probe: SE clock %d reset %d (source %#x), engine not read; fuse pkdisable %#x\n",
                  !!(clocks & kSeClockBit), !!(resets & kSeClockBit), car[kCarClkSourceSe / 4], privateKey);
            goto out;
        }
        IOLog(
            "AppleKeyStore: SE probe: SE1 security %#x perkey %#x access14 %#x access15 %#x status %#x err %#x; SE2 "
            "security %#x perkey %#x access14 %#x status %#x err %#x; fuse pkdisable %#x; MC smmu %#x ppcs-asid %#x\n",
            se1.r(kSecurity), se1.r(kPerKey), se1.r(kKeyAccess + 4 * 14), se1.r(kKeyAccess + 4 * 15), se1.r(kStatus),
            se1.r(kErrStatus), se2.r(kSecurity), se2.r(kPerKey), se2.r(kKeyAccess + 4 * 14), se2.r(kStatus),
            se2.r(kErrStatus), privateKey, smmu, ppcs);
        if ((smmu & kSmmuEnable) && (ppcs & kAsidEnable)) {
            IOLog("AppleKeyStore: SE probe: SMMU translates the SE's AHB DMA (PPCS), no operation run\n");
            goto out;
        }
        if (!allocateDma(&dma)) {
            IOLog("AppleKeyStore: SE probe: no DMA page below 4 GiB\n");
            goto out;
        }
        // SE1 and SE2 are TegraSE's AES/SHA engines too (drivers/TegraSE): one owner at a time.
        if (tegra_se_claim(1, TEGRA_SE_OWNER_AKS_PROBE, 1000)) {
            probeSlot(se1, dma, ahb, 14, r14);
            probeSlot(se1, dma, ahb, 15, r15);
            tegra_se_release(1, TEGRA_SE_OWNER_AKS_PROBE);
        } else {
            r14.refusal = r15.refusal = "SE1 held by TegraSE";
        }
        if (tegra_se_claim(2, TEGRA_SE_OWNER_AKS_PROBE, 1000)) {
            probeSlot(se2, dma, ahb, 14, r2);
            tegra_se_release(2, TEGRA_SE_OWNER_AKS_PROBE);
        } else {
            r2.refusal = "SE2 held by TegraSE";
        }

        // What an empty (all-zero) slot would give, so a slot the boot ROM never loaded is told from a real key.
        uint8_t zeroKeyMac[16], zeroKey[16] = {};
        aes_encrypt_ctx zeroCtx;
        aes_encrypt_key128(zeroKey, &zeroCtx);
        aks::cmac(softwareEncrypt, &zeroCtx, reinterpret_cast<const uint8_t *>(kProbeLabel), sizeof kProbeLabel - 1,
                  zeroKeyMac);
        char a[96], b[96], c[96];
        describe(a, sizeof a, "SE1.14", r14, zeroKeyMac);
        describe(b, sizeof b, "SE1.15", r15, zeroKeyMac);
        describe(c, sizeof c, "SE2.14", r2, zeroKeyMac);
        IOLog("AppleKeyStore: SE probe: %s; %s; %s; SE1.14 differs from SE1.15 %d\n", a, b, c,
              r14.ran && r15.ran && memcmp(r14.mac, r15.mac, 16) != 0);
        aks::wipe(&zeroCtx, sizeof zeroCtx);
    }
out:
    aks::wipe(r14.mac, sizeof r14.mac);
    aks::wipe(r15.mac, sizeof r15.mac);
    aks::wipe(r2.mac, sizeof r2.mac);
    releaseDma(&dma);
    OSSafeReleaseNULL(se2Map);
    OSSafeReleaseNULL(se1Map);
    OSSafeReleaseNULL(mcMap);
    OSSafeReleaseNULL(fuseMap);
    OSSafeReleaseNULL(ahbMap);
    OSSafeReleaseNULL(carMap);
}
