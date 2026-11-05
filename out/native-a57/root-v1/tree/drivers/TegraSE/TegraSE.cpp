// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE: the T210B01 Security Engine SE2 (0x70412000, GIC SPI 190 = INTID 222, L4T tegra210b01-soc-base.dtsi) for the
// kernel. Never booted (r17-crypto). What it does, in order, on its own thread:
//   1. checks: SE clock on and out of reset, SE_SECURITY non-secure, the interrupt attached; SE2 claimed from the
//      AppleKeyStore stand-in's boot probe (TegraSE.h). Each failure is one IOLog line and the driver off for the boot.
//   2. known answers through the interrupt: SHA-256 and SHA-512 of "abc", AES-128-CBC and -CTR of NIST SP 800-38A
//      F.2.1/F.5.1. The first operation also proves INTID 222 reaches us (ioswitch_interrupt_count): an operation the
//      engine shows done whose interrupt never came is the never-booted path's one line, not a panic.
//   3. reseeds the kernel PRNG (cckprng, write_random) with 64 bytes of SE2's SP 800-90 DRBG (entropy source, forced
//      reseed), on top of the per-boot /chosen/random-seed the loader mixed (native/loader/seed.h).
//   4. 90 s later, when the boot is done: SE2 against the A57s' crypto instructions (kernel corecrypto) for SHA-256,
//      SHA-512 and AES-128-CBC at 4/16/64/256 KiB, each SE result checked against the CPU's, and logs per-operation
//      times: engine (start to the interrupt), setup (programming, before the start) and wake (interrupt to the
//      waiting thread running), and the CPU's. These are the numbers the offload decisions in r17-crypto's report need;
//      nothing routes kernel crypto here yet.
//
// Registers and sequences: Linux L4T drivers/crypto/tegra-se.c (NVIDIA; GPL-2.0) tegra_se_config_algo/config_crypto/
// config_sha/start_operation/read_hash_result/write_key_table with the tegra210b01 chip data (ahb_ack); hekate bdk/sec
// se.c and se_t210.h (CTCaer, naehrwert; GPL-2.0) for the key table and SHA; switchroot BL31 security_engine.c. Written
// from the register facts in those sources, no code copied. The engine takes 32-bit addresses and does not snoop the
// CPU caches: its buffers are physically contiguous below 4 GiB, cleaned before it reads and invalidated before the CPU
// reads what it wrote. Memory interface AHB (the path r11-sep's probe proved on hardware), not MCCIF. Keyslot 8 holds
// the test key and is zeroed after (L4T leaves slots 1..14 to its users; 0 is its SRK slot, 14/15 are Mariko's boot keys
// and the AppleKeyStore stand-in's).
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
#include <string.h>
#include "TegraSE.h"
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" {
extern task_t kernel_task;
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
int write_random(void *buffer, u_int numBytes);
kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
void thread_deallocate(thread_t thread);
}

namespace {
constexpr vm_offset_t kSe2PA = 0x70412000, kCarPA = 0x60006000, kAhbArbPA = 0x6000c000;
constexpr uint32_t kIntid = 32 + 190;
enum : uint32_t {
    SECURITY = 0x000,
    OPERATION = 0x008,
    INT_ENABLE = 0x00c,
    INT_STATUS = 0x010,
    CONFIG = 0x014,
    IN_LL = 0x018,
    OUT_LL = 0x024,
    HASH_RESULT = 0x030,
    SHA_CONFIG = 0x200,
    SHA_MSG_LENGTH = 0x204,
    SHA_MSG_LEFT = 0x214,
    PERKEY = 0x280,
    KEY_ACCESS = 0x284,
    CRYPTO_CONFIG = 0x304,
    CRYPTO_CTR = 0x308,
    LAST_BLOCK = 0x318,
    KEYTABLE_ADDR = 0x31c,
    KEYTABLE_DATA = 0x320,
    RNG_CONFIG = 0x340,
    RNG_SRC_CONFIG = 0x344,
    STATUS = 0x800,
    ERR_STATUS = 0x804,
    SPARE = 0x80c,
};
constexpr uint32_t kSoftNonsecure = 1u << 16, kEngineDisabled = 1u << 1;
constexpr uint32_t kIntDone = 1u << 4, kIntError = 1u << 16;
constexpr uint32_t kStatusBusy = 7;  // STATE not idle, or MEM_IF_BUSY (T210B01)
constexpr uint32_t kAhbWrque = 0x0fc, kAhbMasterSe2 = 1u << 13;
constexpr uint32_t kCarRstV = 0x358, kCarClkV = 0x360, kCarRstW = 0x35c, kCarClkW = 0x364;
constexpr uint32_t kCarSe = 1u << 31, kCarEntropy = 1u << 21;

// CONFIG: ENC_ALG << 12 (AES 1, RNG 2, SHA 3), ENC_MODE << 24 (AES key 0 = 128; SHA256 5, SHA512 7), DST << 2.
constexpr uint32_t kConfigSha256 = 3u << 12 | 5u << 24 | 1u << 2, kConfigSha512 = 3u << 12 | 7u << 24 | 1u << 2;
constexpr uint32_t kConfigAesEncrypt = 1u << 12, kConfigRng = 2u << 12 | 1u << 24;
// CRYPTO_CONFIG fields: XOR_POS << 1 (bypass 0, top 2, bottom 3), INPUT_SEL << 3 (AHB 0, RANDOM 1, AESOUT 2,
// LNR_CTR 3), VCTRAM_SEL << 5 (AHB 0, AESOUT 2, PREVAHB 3), IV_SEL << 7, CORE_SEL << 8 (encrypt 1), CTR_CNTN << 11,
// KEY_INDEX << 24, MEMIF << 31 (AHB 0).
constexpr uint32_t kSlot = 8;
constexpr uint32_t kCryptoCbcEncrypt = 2u << 1 | 2u << 5 | 1u << 8 | kSlot << 24;
constexpr uint32_t kCryptoCtr = 3u << 1 | 3u << 3 | 1u << 8 | 1u << 11 | kSlot << 24;
constexpr uint32_t kCryptoRng = 1u << 3 | 1u << 8;
// KEY_ACCESS allows: KEYUPDATE 1, OIVUPDATE 3, KEYUSE 6.
constexpr uint32_t kSlotAccessNeeded = 1u << 1 | 1u << 3 | 1u << 6;

constexpr uint32_t kBufferBytes = 256 * 1024;
constexpr uint32_t kOpTimeoutMs = 100;  // 256 KiB takes about a millisecond

volatile uint8_t *gSe, *gCar, *gAhb;
IOWorkLoop *gLoop;
IOFilterInterruptEventSource *gSource;
volatile uint32_t gSeen, gStatus;  // the filter's: INT_STATUS it acknowledged
volatile uint64_t gDoneAt;
int gEvent;
volatile uint32_t gOwner;

struct Dma {
    IOBufferMemoryDescriptor *memory;
    uint8_t *va;
    uint32_t pa;
};
Dma gIn, gOut, gList;  // source, destination, the two lists (+0 in, +0x40 out)

uint32_t rd(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(gSe + offset);
}
void wr(uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(gSe + offset) = value;
}
uint32_t car(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(gCar + offset);
}

uint64_t us(uint64_t ticks)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 1000;
}
uint64_t tenths(uint64_t ticks)  // of a microsecond
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 100;
}

void cleanInvalidate(const void *va, size_t size)
{
    for (uintptr_t a = (uintptr_t)va & ~(uintptr_t)63, end = (uintptr_t)va + size; a < end; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

bool allocate(Dma *dma, uint32_t size)
{
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous;
    dma->memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, size, 0x00000000fffff000ULL);
    if (!dma->memory) return false;
    IOByteCount length = 0;
    const addr64_t pa = dma->memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!pa || length < size || pa + size > 0x100000000ULL) {
        OSSafeReleaseNULL(dma->memory);
        return false;
    }
    dma->va = static_cast<uint8_t *>(dma->memory->getBytesNoCopy());
    dma->pa = (uint32_t)pa;
    bzero(dma->va, size);
    cleanInvalidate(dma->va, size);
    return true;
}

void release(Dma *dma)
{
    if (!dma->memory) return;
    bzero(dma->va, dma->memory->getLength());
    cleanInvalidate(dma->va, dma->memory->getLength());
    OSSafeReleaseNULL(dma->memory);
}

// Primary interrupt context: an operation's end (or error) acknowledged, the waiting thread woken. Nothing else.
bool filter(OSObject *, IOFilterInterruptEventSource *)
{
    const uint32_t status = rd(INT_STATUS);
    if (!(status & (kIntDone | kIntError))) return false;
    wr(INT_STATUS, status);
    gDoneAt = mach_absolute_time();
    gStatus = status;
    gSeen = gSeen + 1;
    thread_wakeup(&gEvent);
    return false;
}

struct Times {
    uint64_t setup, engine, wake;  // absolute-time units
};
char gWhy[200];

// One operation, programmed by the caller up to the lists; started here, waited for on the interrupt.
bool run(uint32_t in, uint32_t out, uint64_t begun, Times *t)
{
    wr(IN_LL, in);
    wr(OUT_LL, out);
    wr(ERR_STATUS, rd(ERR_STATUS));
    wr(INT_STATUS, rd(INT_STATUS));
    if (rd(OUT_LL) != out || rd(IN_LL) != in) {
        snprintf(gWhy, sizeof gWhy, "list registers did not take (IN_LL %#x OUT_LL %#x)", rd(IN_LL), rd(OUT_LL));
        return false;
    }
    if (rd(STATUS) & kStatusBusy) {
        snprintf(gWhy, sizeof gWhy, "engine busy before start (STATUS %#x)", rd(STATUS));
        return false;
    }
    const uint32_t seen = gSeen;
    uint64_t interval;
    nanoseconds_to_absolutetime((uint64_t)kOpTimeoutMs * 1000000, &interval);
    assert_wait_deadline(&gEvent, THREAD_UNINT, mach_absolute_time() + interval);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t started = mach_absolute_time();
    wr(OPERATION, 1);
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
    const uint64_t woken = mach_absolute_time();
    if (result == THREAD_TIMED_OUT || gSeen == seen) {
        const uint32_t status = rd(INT_STATUS);
        char gic[160];
        ioswitch_interrupt_describe(kIntid, gic, sizeof gic);
        snprintf(gWhy, sizeof gWhy, "%s within %u ms (INT_STATUS %#x STATUS %#x ERR %#x; %s)",
                 (status & (kIntDone | kIntError)) ? "operation done, interrupt not delivered" : "no operation end",
                 kOpTimeoutMs, status, rd(STATUS), rd(ERR_STATUS), gic);
        return false;
    }
    const uint32_t error = rd(ERR_STATUS);
    if ((gStatus & kIntError) || error) {
        wr(ERR_STATUS, error);
        snprintf(gWhy, sizeof gWhy, "operation error (INT_STATUS %#x ERR_STATUS %#x)", gStatus, error);
        return false;
    }
    // T210B01: the output is in memory once the memory interface is quiet and the AHB write queue drained (L4T
    // ahb_ack, hekate _se_op_wait). There is no interrupt for that; it is microseconds after the end at most.
    for (unsigned i = 0;
         (rd(STATUS) & kStatusBusy) || (*reinterpret_cast<volatile uint32_t *>(gAhb + kAhbWrque) & kAhbMasterSe2);
         ++i) {
        if (i > 100000) {
            snprintf(gWhy, sizeof gWhy, "output never drained (STATUS %#x)", rd(STATUS));
            return false;
        }
    }
    if (t) {
        t->setup = started - begun;
        t->engine = gDoneAt - started;
        t->wake = woken - gDoneAt;
    }
    return true;
}

void setList(uint32_t at, uint32_t pa, uint32_t bytes)
{
    uint32_t *list = reinterpret_cast<uint32_t *>(gList.va + at);
    list[0] = 0;  // last entry index
    list[1] = pa;
    list[2] = bytes;
    cleanInvalidate(list, 16);
}

// One-shot SHA-256 (wide false) or SHA-512 of gIn.va[0, bytes).
bool sha(bool wide, uint32_t bytes, uint8_t *digest, Times *t)
{
    const uint64_t begun = mach_absolute_time();
    cleanInvalidate(gIn.va, bytes);
    setList(0, gIn.pa, bytes);
    wr(CONFIG, wide ? kConfigSha512 : kConfigSha256);
    wr(CRYPTO_CONFIG, 0);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t bits = i == 0 ? bytes << 3 : i == 1 ? bytes >> 29 : 0;
        wr(SHA_MSG_LENGTH + 4 * i, bits);
        wr(SHA_MSG_LEFT + 4 * i, bits);
    }
    wr(SHA_CONFIG, 1);  // INIT_HASH: a new message
    if (!run(gList.pa, 0, begun, t)) return false;
    const unsigned words = wide ? 16 : 8;
    uint32_t w[16];
    for (unsigned i = 0; i < words; ++i) w[i] = __builtin_bswap32(rd(HASH_RESULT + 4 * i));
    if (wide) {  // the 64-bit words come out low half first (L4T tegra_se_sha_final)
        for (unsigned i = 0; i < words; i += 2) {
            const uint32_t x = w[i];
            w[i] = w[i + 1];
            w[i + 1] = x;
        }
    }
    memcpy(digest, w, words * 4);
    return true;
}

void writeSlot(unsigned quad, const uint8_t *bytes)
{
    uint32_t w[4];
    memcpy(w, bytes, 16);
    for (unsigned i = 0; i < 4; ++i) {
        wr(KEYTABLE_ADDR, kSlot << 4 | quad << 2 | i);
        wr(KEYTABLE_DATA, w[i]);
    }
}

void setKey(const uint8_t key[16], const uint8_t iv[16])
{
    static const uint8_t zero[16] = {};
    writeSlot(0, key);
    writeSlot(1, zero);  // words 4-7 (AES-192/256 halves)
    writeSlot(2, iv);    // original IV
}

// AES-128 CBC encryption (ctr false) or CTR of gIn.va[0, bytes) into gOut.va; the key and IV/counter given.
bool aes(bool ctr, uint32_t bytes, const uint8_t key[16], const uint8_t iv[16], Times *t)
{
    const uint64_t begun = mach_absolute_time();
    cleanInvalidate(gIn.va, bytes);
    cleanInvalidate(gOut.va, bytes);
    setKey(key, iv);
    setList(0, gIn.pa, bytes);
    setList(0x40, gOut.pa, bytes);
    wr(CONFIG, kConfigAesEncrypt);
    wr(CRYPTO_CONFIG, ctr ? kCryptoCtr : kCryptoCbcEncrypt);
    if (ctr) {
        uint32_t w[4];
        memcpy(w, iv, 16);
        for (unsigned i = 0; i < 4; ++i) wr(CRYPTO_CTR + 4 * i, w[i]);
        wr(SPARE, 1);  // L4T sets it for CTR and OFB
    }
    wr(LAST_BLOCK, bytes / 16 - 1);
    const bool ok = run(gList.pa, gList.pa + 0x40, begun, t);
    if (ctr) wr(SPARE, 0);
    cleanInvalidate(gOut.va, bytes);
    return ok;
}

bool rng(uint8_t out[64])
{
    if (!(car(kCarClkW) & kCarEntropy) || (car(kCarRstW) & kCarEntropy)) {
        snprintf(gWhy, sizeof gWhy, "entropy clock off (CLK_OUT_ENB_W %#x)", car(kCarClkW));
        return false;
    }
    cleanInvalidate(gOut.va, 64);
    setList(0x40, gOut.pa, 64);
    wr(CONFIG, kConfigRng);
    wr(CRYPTO_CONFIG, kCryptoRng);
    wr(RNG_CONFIG, 1u << 2 | 2u);  // SRC_ENTROPY | FORCE_RESEED
    wr(RNG_SRC_CONFIG, rd(RNG_SRC_CONFIG) | 1u << 1);
    wr(LAST_BLOCK, 3);
    if (!run(0, gList.pa + 0x40, mach_absolute_time(), nullptr)) return false;
    cleanInvalidate(gOut.va, 64);
    memcpy(out, gOut.va, 64);
    bzero(gOut.va, 64);
    cleanInvalidate(gOut.va, 64);
    uint32_t any = 0;
    for (unsigned i = 0; i < 64; ++i) any |= out[i];
    bool repeat = false;
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = a + 1; b < 4; ++b) repeat |= !memcmp(out + 16 * a, out + 16 * b, 16);
    if (!any || repeat) {
        snprintf(gWhy, sizeof gWhy, "DRBG output implausible (zero %d, repeated block %d)", !any, repeat);
        return false;
    }
    return true;
}

void cpuSha(bool wide, const void *data, uint32_t bytes, uint8_t *digest)
{
    if (wide) {
        SHA512_CTX c;
        SHA512_Init(&c);
        SHA512_Update(&c, data, bytes);
        SHA512_Final(digest, &c);
    } else {
        SHA256_CTX c;
        SHA256_Init(&c);
        SHA256_Update(&c, data, bytes);
        SHA256_Final(digest, &c);
    }
}

const uint8_t kKey[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                          0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
const uint8_t kPlain[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
                            0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
                            0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
                            0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
                            0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
const uint8_t kCbcIv[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const uint8_t kCbc[16] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
                          0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};  // F.2.1 block 1
const uint8_t kCtrIv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                            0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
const uint8_t kCtr4[16] = {0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1,
                           0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee};  // F.5.1 block 4: the counter carried

// The known answers; false with gWhy.
bool knownAnswers()
{
    static const uint8_t abc256[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                       0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                       0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    static const uint8_t abc512[16] = {0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
                                       0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31};  // the first 16 of 64
    uint8_t digest[64];
    memcpy(gIn.va, "abc", 3);
    if (!sha(false, 3, digest, nullptr)) return false;
    if (memcmp(digest, abc256, 32)) {
        snprintf(gWhy, sizeof gWhy, "SHA-256(abc) wrong (%02x%02x%02x%02x...)", digest[0], digest[1], digest[2],
                 digest[3]);
        return false;
    }
    if (!sha(true, 3, digest, nullptr)) return false;
    if (memcmp(digest, abc512, 16)) {
        snprintf(gWhy, sizeof gWhy, "SHA-512(abc) wrong (%02x%02x%02x%02x...)", digest[0], digest[1], digest[2],
                 digest[3]);
        return false;
    }
    memcpy(gIn.va, kPlain, 64);
    if (!aes(false, 64, kKey, kCbcIv, nullptr)) return false;
    if (memcmp(gOut.va, kCbc, 16)) {
        snprintf(gWhy, sizeof gWhy, "AES-128-CBC F.2.1 wrong (%02x%02x%02x%02x...)", gOut.va[0], gOut.va[1], gOut.va[2],
                 gOut.va[3]);
        return false;
    }
    if (!aes(true, 64, kKey, kCtrIv, nullptr)) return false;
    if (memcmp(gOut.va + 48, kCtr4, 16)) {
        snprintf(gWhy, sizeof gWhy, "AES-128-CTR F.5.1 block 4 wrong (%02x%02x%02x%02x...)", gOut.va[48], gOut.va[49],
                 gOut.va[50], gOut.va[51]);
        return false;
    }
    return true;
}

void clearSlot()
{
    static const uint8_t zero[16] = {};
    setKey(zero, zero);
}

// Times SE2 against the CPU at four sizes; one line per algorithm. Every SE result is checked against the CPU's.
bool calibrate()
{
    static const uint32_t sizes[4] = {4096, 16384, 65536, 262144};
    for (uint32_t i = 0; i < kBufferBytes; i += 4) {  // something that is not zeros
        const uint32_t x = i * 2654435761u ^ 0x5bd1e995u;
        memcpy(gIn.va + i, &x, 4);
    }
    uint8_t *cpuOut = static_cast<uint8_t *>(IOMalloc(kBufferBytes));
    if (!cpuOut) {
        snprintf(gWhy, sizeof gWhy, "no memory for the CPU's copy");
        return false;
    }
    bool ok = true;
    static const char *const names[3] = {"sha256", "sha512", "aes128-cbc"};
    for (unsigned algo = 0; ok && algo < 3; ++algo) {
        char line[240];
        int at = snprintf(line, sizeof line, "TegraSE: %s us engine/setup/wake vs cpu:", names[algo]);
        for (unsigned s = 0; s < 4; ++s) {
            const uint32_t n = sizes[s];
            Times best = {~0ull, ~0ull, ~0ull};
            uint64_t cpuBest = ~0ull;
            uint8_t se[64], cpu[64];
            for (unsigned rep = 0; ok && rep < 5; ++rep) {
                Times t;
                ok = algo < 2 ? sha(algo == 1, n, se, &t) : aes(false, n, kKey, kCbcIv, &t);
                if (!ok) break;
                if (t.engine < best.engine) best.engine = t.engine;
                if (t.setup < best.setup) best.setup = t.setup;
                if (t.wake < best.wake) best.wake = t.wake;
                const uint64_t c0 = mach_absolute_time();
                if (algo < 2) {
                    cpuSha(algo == 1, gIn.va, n, cpu);
                } else {
                    aes_encrypt_ctx context;
                    aes_encrypt_key128(kKey, &context);
                    aes_encrypt_cbc(gIn.va, kCbcIv, n / 16, cpuOut, &context);
                    bzero(&context, sizeof context);
                }
                const uint64_t c1 = mach_absolute_time() - c0;
                if (c1 < cpuBest) cpuBest = c1;
            }
            if (!ok) break;
            // The SE's answer against the CPU's, every size.
            if (algo < 2 ? memcmp(se, cpu, algo == 1 ? 64 : 32) != 0 : memcmp(gOut.va, cpuOut, n) != 0) {
                snprintf(gWhy, sizeof gWhy, "%s of %u bytes differs from the CPU's", names[algo], n);
                ok = false;
                break;
            }
            const uint64_t e = tenths(best.engine), u = tenths(best.setup), w = tenths(best.wake), c = tenths(cpuBest);
            if (at > 0 && (size_t)at < sizeof line)
                at += snprintf(line + at, sizeof line - (size_t)at, " %uK %llu.%llu/%llu.%llu/%llu.%llu vs %llu.%llu%s",
                               n / 1024, e / 10, e % 10, u / 10, u % 10, w / 10, w % 10, c / 10, c % 10,
                               s < 3 ? "," : "");
        }
        if (ok) IOLog("%s\n", line);
    }
    bzero(cpuOut, kBufferBytes);
    IOFree(cpuOut, kBufferBytes);
    return ok;
}

// The one line, and everything given back. `owned`: SE2 is still claimed (its interrupt and test key are undone first).
void off(const char *step, bool owned)
{
    IOLog("TegraSE: off for this boot: %s: %s\n", step, gWhy);
    if (owned) {
        wr(INT_ENABLE, 0);
        clearSlot();
        tegra_se2_release(TEGRA_SE2_OWNER_DRIVER);
    }
    if (gSource) {
        gSource->disable();
        if (gLoop) gLoop->removeEventSource(gSource);
        OSSafeReleaseNULL(gSource);
    }
    release(&gIn);
    release(&gOut);
    release(&gList);
}

void seThread(void *, wait_result_t)
{
    gSe = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSe2PA, 0x2000));
    gCar = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    gAhb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kAhbArbPA, 0x1000));
    if (!gSe || !gCar || !gAhb) {
        snprintf(gWhy, sizeof gWhy, "cannot map SE2/CAR/AHB");
        return off("map", false);
    }
    // A clock-gated or reset APB block can hang the bus: no engine register is read unless the clock runs.
    if (!(car(kCarClkV) & kCarSe) || (car(kCarRstV) & kCarSe)) {
        snprintf(gWhy, sizeof gWhy, "SE clock off or in reset (CLK_OUT_ENB_V %#x RST_DEVICES_V %#x)", car(kCarClkV),
                 car(kCarRstV));
        return off("clock", false);
    }
    if (!tegra_se2_claim(TEGRA_SE2_OWNER_DRIVER, 10000)) {
        snprintf(gWhy, sizeof gWhy, "SE2 still held by the AppleKeyStore probe after 10 s");
        return off("claim", false);
    }
    const uint32_t security = rd(SECURITY), perkey = rd(PERKEY), access = rd(KEY_ACCESS + 4 * kSlot);
    if (!(security & kSoftNonsecure) || (security & kEngineDisabled) || !(perkey & (1u << kSlot)) ||
        (access & kSlotAccessNeeded) != kSlotAccessNeeded) {
        snprintf(gWhy, sizeof gWhy, "SE2 not usable from EL1 (SECURITY %#x PERKEY %#x slot %u access %#x)", security,
                 perkey, kSlot, access);
        tegra_se2_release(TEGRA_SE2_OWNER_DRIVER);
        return off("security", false);
    }
    if (!allocate(&gIn, kBufferBytes) || !allocate(&gOut, kBufferBytes) || !allocate(&gList, PAGE_SIZE)) {
        snprintf(gWhy, sizeof gWhy, "no physically contiguous buffers below 4 GiB");
        return off("memory", true);
    }
    IOService *nub = ioswitch_interrupt_nub(kIntid, false);  // sleeps until the GIC is up
    gLoop = IOWorkLoop::workLoop();
    gSource = nub && gLoop ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, nullptr, &filter, nub, 0)
                           : nullptr;
    if (!gSource || gLoop->addEventSource(gSource) != kIOReturnSuccess) {
        snprintf(gWhy, sizeof gWhy, "cannot attach INTID %u", kIntid);
        return off("interrupt", true);
    }
    wr(INT_STATUS, rd(INT_STATUS));
    wr(INT_ENABLE, kIntDone | kIntError);
    gSource->enable();

    const uint64_t kat0 = mach_absolute_time();
    if (!knownAnswers()) return off("known answers", true);
    const uint64_t katUs = us(mach_absolute_time() - kat0);
    uint8_t seed[64];
    const bool reseeded = rng(seed);
    if (reseeded) write_random(seed, sizeof seed);
    bzero(seed, sizeof seed);
    wr(INT_ENABLE, 0);
    clearSlot();
    tegra_se2_release(TEGRA_SE2_OWNER_DRIVER);
    IOLog("TegraSE: SE2 up: known answers (SHA-256, SHA-512, AES-128-CBC/CTR) in %llu us over INTID %u (%u "
          "interrupts); kernel PRNG %s\n",
          katUs, kIntid, ioswitch_interrupt_count(kIntid), reseeded ? "reseeded from the SE2 DRBG" : gWhy);

    // The timing run waits for the boot to be over: it takes a few hundred milliseconds of one core.
    IOSleep(90000);
    // System sleep (SC7) in between turns the SE clock off and holds the engine in reset (BL31 tegra_se_suspend): check
    // again before any engine register is touched, or the read hangs the bus.
    if (!(car(kCarClkV) & kCarSe) || (car(kCarRstV) & kCarSe)) {
        snprintf(gWhy, sizeof gWhy,
                 "SE clock off or in reset before the timing run (CLK_OUT_ENB_V %#x RST_DEVICES_V %#x)", car(kCarClkV),
                 car(kCarRstV));
        return off("timing", false);
    }
    if (!tegra_se2_claim(TEGRA_SE2_OWNER_DRIVER, 10000)) {
        snprintf(gWhy, sizeof gWhy, "SE2 held by another owner");
        return off("timing", false);
    }
    wr(INT_STATUS, rd(INT_STATUS));
    wr(INT_ENABLE, kIntDone | kIntError);
    if (!calibrate()) return off("timing", true);
    wr(INT_ENABLE, 0);
    clearSlot();
    tegra_se2_release(TEGRA_SE2_OWNER_DRIVER);
    gSource->disable();
    gLoop->removeEventSource(gSource);
    OSSafeReleaseNULL(gSource);
    release(&gIn);
    release(&gOut);
    release(&gList);
    IOLog("TegraSE: timing done, SE2 idle (nothing routes kernel crypto to it yet)\n");
}
}  // namespace

extern "C" bool tegra_se2_claim(uint32_t owner, uint32_t ms)
{
    for (uint32_t waited = 0;; ++waited) {
        if (__sync_bool_compare_and_swap(&gOwner, 0u, owner)) return true;
        if (gOwner == owner) return true;
        if (waited >= ms) return false;
        IOSleep(1);
    }
}

extern "C" void tegra_se2_release(uint32_t owner)
{
    (void)__sync_bool_compare_and_swap(&gOwner, owner, 0u);
}

extern "C" int tegra_se_start(void)
{
    thread_t t;
    if (kernel_thread_start(seThread, nullptr, &t) != KERN_SUCCESS) return -1;
    thread_deallocate(t);
    return 0;
}
