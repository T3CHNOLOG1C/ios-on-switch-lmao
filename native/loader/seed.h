#ifndef IOSWITCH_LOADER_SEED_H
#define IOSWITCH_LOADER_SEED_H
/* A per-boot /chosen/random-seed from the Security Engine's DRBG.
 *
 * XNU seeds early_random() and the kernel PRNG (cckprng) from the 64 bytes of /chosen/random-seed and a timebase nonce
 * (osfmk/prng/prng_random.c bootseed_init; the arm64 bootseed_init_native is empty), then zeroes the property
 * (PE_get_random_seed). The ADT carries a seed fixed at package time (tools/package/adt.py pins it), so without this
 * every boot of every build starts both generators from the same bytes. Just before XNU starts, with the caches and MMU
 * off, the loader XORs 64 bytes of SE2's SP 800-90 DRBG output (entropy source on, forced reseed) and the counter into
 * the seed in the copied ADT. XOR keeps the result at least as good as either input: if the engine does not answer,
 * the seed is the pinned one as before, and the loader never refuses for this.
 *
 * Programming (Linux L4T tegra-se.c tegra_se_config_algo/config_crypto SE_AES_OP_MODE_RNG_DRBG; hekate bdk/sec/se.c
 * se_rng_pseudo and _se_op_wait; switchroot BL31 tegra_se_generate_srk): CONFIG = ENC_ALG RNG | ENC_MODE KEY192 | DST
 * MEMORY, CRYPTO_CONFIG = INPUT_SEL RANDOM | XOR_BYPASS | CORE_SEL ENCRYPT (AHB memory interface), RNG_CONFIG = SRC
 * ENTROPY | MODE FORCE_RESEED (FORCE_INSTANTIATION once if that errors), RNG_SRC_CONFIG |= RO_ENT_SRC (never its lock
 * bit), LAST_BLOCK = 3, an empty input list and a one-entry output list. The DRBG's entropy source needs the ENTROPY
 * clock (CLK_W bit 21); BL31 turns it off across SC7, so the loader turns it on (and out of reset) if it finds it off,
 * as Linux's clk_prepare_enable(entropy) does. SE2 (0x70412000) is used: SE1's keyslots are the AppleKeyStore stand-in's
 * (drivers/NativeKeyStoreEvents/TegraSecurityEngine.cpp), and an RNG operation touches no keyslot anyway. Polled: there
 * is no interrupt controller yet (like that file's boot probe); each wait is bounded by the counter.
 *
 * The result goes to /chosen/ioswitch-seed-source (two LE32 words: IOSW_SEED_MAGIC, status | detail << 16) and XNU logs
 * it once (osfmk/kern/startup.c ioswitch_log_boot_stamps). The detail of a mixed seed is a 16-bit fold of the engine's
 * output, so two boots can be told apart in the log; of a failure, the low 16 bits of the register that showed it. */
#include <stdint.h>

#define IOSW_SEED_MAGIC 0x44454553u /* "SEED" */
#define IOSW_SEED_BYTES 64u
#define IOSW_SEED_SOURCE_BYTES 8u

enum iosw_seed_status {
    IOSW_SEED_STATUS_NONE = 0,         /* the loader never ran this (an older loader) */
    IOSW_SEED_STATUS_MIXED = 1,        /* SE2 DRBG output mixed in */
    IOSW_SEED_STATUS_SE_CLOCK = 0x10,  /* SE clock off or the engine in reset: not touched */
    IOSW_SEED_STATUS_SECURE = 0x11,    /* SE_SECURITY: secure-only or engine disabled (detail: SE_SECURITY) */
    IOSW_SEED_STATUS_BUSY = 0x12,      /* engine not idle before start (detail: SE_STATUS) */
    IOSW_SEED_STATUS_IGNORED = 0x13,   /* configuration writes did not take (detail: CONFIG read back) */
    IOSW_SEED_STATUS_TIMEOUT = 0x14,   /* no OP_DONE in time (detail: INT_STATUS) */
    IOSW_SEED_STATUS_ERROR = 0x15,     /* error bit or ERR_STATUS (detail: ERR_STATUS, or INT_STATUS) */
    IOSW_SEED_STATUS_DRAIN = 0x16,     /* memory interface or AHB write queue not drained (detail: SE_STATUS) */
    IOSW_SEED_STATUS_OUTPUT = 0x17,    /* output all zero or blocks repeating: not used */
    IOSW_SEED_STATUS_ENTROPY = 0x18,   /* the entropy clock would not come on (detail: CLK_OUT_ENB_W) */
};
/* Set with a status when the entropy clock was off and the loader turned it on. */
#define IOSW_SEED_FLAG_ENTROPY_ENABLED 0x100u

/* Hardware the module touches (physical; MMU off). */
#define IOSW_SEED_CAR_PA UINT64_C(0x60006000)
#define IOSW_SEED_AHB_ARB_PA UINT64_C(0x6000c000)
#define IOSW_SEED_SE2_PA UINT64_C(0x70412000)

/* Mixes the engine's output and `counter` into the 64-byte seed at seed_pa (4-byte aligned, 32-bit accesses) and writes
 * the two status words at source_pa. `ticks` reads the counter; `timeout` is in its units. Returns the status word. */
uint32_t iosw_seed_mix(uint64_t seed_pa, uint64_t source_pa, uint64_t (*ticks)(void), uint64_t timeout);

/* Supplied by the target (seed.c, MMIO) or by the host test. */
uint32_t iosw_seed_read32(uint64_t pa);
void iosw_seed_write32(uint64_t pa, uint32_t value);
#endif
