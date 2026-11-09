/* EMC raise before the kernel window copy: emc.h says what and why. Runs with the MMU off (every access below is an
 * aligned 32-bit device access) and interrupts masked; every wait is bounded on CNTPCT. */
#include "emc.h"

#define SEMA        UINT64_C(0x60001000) /* arbitration semaphore: STA +0, SET +4, CLR +8 (mail_t210.c) */
#define ATOMICS     UINT64_C(0x70016000) /* TRIGGER +0, RESULT(ch) +0xc00 + 4 ch */
#define LIC_FIR_SET UINT64_C(0x60004018) /* ICTLR CPU_IEP_FIR_SET: bit 6 rings the BPMP (outgoing-doorbell <6>) */
#define CAR         UINT64_C(0x60006000)
#define I2C5        UINT64_C(0x7000d000)
#define HIDREV      UINT64_C(0x70000804) /* APB_MISC_GP_HIDREV: major revision bits 7:4, 2 = T210B01 */

/* Per-CPU atomic channel 0 (nv_bpmp.c t210_chcfg per_cpu_ch_0); the kernel's TegraBPMP uses thread channels 4-7. */
#define CH_MASK         3u
#define MA_FREE         2u
#define MA_ACKD         3u
#define DO_ACK          1u
#define TRIGGER_CMD_GET 4u

#define MRQ_PING             0u
#define MRQ_CLK              22u
#define MRQ_EMC_DVFS_TABLE   32u
#define CLK(cmd)             ((uint32_t)(cmd) << 24) /* BPMP clock 0 is EMC on T210B01 (TegraBPMP.c) */
#define CMD_CLK_GET_RATE     1u
#define CMD_CLK_SET_RATE     2u
#define CMD_CLK_ROUND_RATE   3u
#define CMD_CLK_ENABLE       7u
#define CMD_CLK_GET_ALL_INFO 14u
#define CLK_NAME_OFFSET      73u /* cmd_clk_get_all_info_response: flags, parent, parents[16], num_parents, name[40] */
#define DATA_WORDS           30u /* MSG_DATA_MIN_SZ 120 */

/* I2C5 (PWR_I2C) in packet mode, as the kernel's TegraI2C.cpp adopts it from U-Boot. */
#define I2C_CNFG            0x00u
#define I2C_TX_FIFO         0x50u
#define I2C_RX_FIFO         0x54u
#define I2C_FIFO_CONTROL    0x5cu
#define I2C_FIFO_STATUS     0x60u
#define I2C_INT_MASK        0x64u
#define I2C_INT_STATUS      0x68u
#define I2C_CONFIG_LOAD     0x8cu
#define CNFG_PACKET_MODE    (1u << 10)
#define CNFG_NEW_MASTER_FSM (1u << 11)
#define INT_ARB_LOST        (1u << 2)
#define INT_NO_ACK          (1u << 3)
#define INT_PACKET_DONE     (1u << 7)
#define MAX77620            0x3cu
#define MAX77620_REG_SD0    0x16u /* VDD_CORE: 600 mV + 12.5 mV * bits 6:0 */

/* L4T's VDD_CORE for an EMC rate on T210B01 (TegraBPMP.c emc_core_mv: emcb01_dvb_dvfs_table, process 0 column). */
static const uint32_t emc_core_mv[][2] = {
    {1065600, 637}, {1331200, 650}, {1600000, 675}, {1866000, 700},  {2133000, 725},  {2400000, 750},
    {2533000, 800}, {2666000, 825}, {2800000, 900}, {2933000, 1000}, {3066000, 1025}, {3200000, 1050},
};

#ifdef LOADER_EMC_HOST_TEST
/* test/emc_model.c: a model of the BPMP mailbox, I2C5 and the counter. */
uint32_t rd(uint64_t address);
void wr(uint64_t address, uint32_t value);
uint64_t now(void);
#define dsb() ((void)0)
#else
static uint32_t rd(uint64_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static void wr(uint64_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static uint64_t now(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value)::"memory");
    return value;
}

#define dsb() __asm__ volatile("dsb sy" ::: "memory")
#endif

/* Counter ticks per microsecond. In .data: the image has no BSS (link.ld). */
#ifdef LOADER_EMC_HOST_TEST
static uint64_t per_us;
#else
static uint64_t per_us __attribute__((section(".data"))) = 0;
#endif

/* Until (reg & mask) == want, at most `us` microseconds. 0 when it got there. */
static int wait_for(uint64_t reg, uint32_t mask, uint32_t want, uint64_t us)
{
    const uint64_t end = now() + us * per_us;
    while ((rd(reg) & mask) != want)
        if (now() > end)
            return (rd(reg) & mask) != want;
    return 0;
}

/* One request on channel 0 (mail.c bpmp_send_receive_atomic): 0, or LOADER_EMC_BUSY / LOADER_EMC_NO_ANSWER. *code is
 * the BPMP's return code. */
static uint32_t bpmp_call(uint64_t area, uint32_t mrq, const uint32_t *in, unsigned in_words, uint32_t *out,
                          unsigned out_words, uint64_t us, int32_t *code)
{
    if (wait_for(SEMA, CH_MASK, MA_FREE, 10000))
        return LOADER_EMC_BUSY;
    wr(area, mrq);
    wr(area + 4, DO_ACK);
    for (unsigned i = 0; i < in_words; ++i)
        wr(area + 8 + 4 * i, in[i]);
    dsb();
    wr(SEMA + 8, CH_MASK);    /* signal_slave: MA_FREE -> SL_SIGL */
    wr(LIC_FIR_SET, 1u << 6); /* ring_doorbell */
    if (wait_for(SEMA, CH_MASK, MA_ACKD, us))
        return LOADER_EMC_NO_ANSWER;
    *code = (int32_t)rd(area);
    for (unsigned i = 0; i < out_words; ++i)
        out[i] = rd(area + 8 + 4 * i);
    wr(SEMA + 8, MA_ACKD ^ MA_FREE); /* free_master: MA_ACKD -> MA_FREE */
    return 0;
}

/* One I2C5 packet, polled (TegraI2C.cpp pollPacket): a one-byte write, or a one-byte read into *in. 0 on success. */
static int i2c_packet(uint32_t address, int read, uint32_t out, uint32_t *in, int repeat_start)
{
    wr(I2C5 + I2C_INT_STATUS, rd(I2C5 + I2C_INT_STATUS));
    wr(I2C5 + I2C_FIFO_CONTROL, (7u << 5) | 3u);
    if (wait_for(I2C5 + I2C_FIFO_CONTROL, 3u, 0, 2000))
        return 1;
    wr(I2C5 + I2C_TX_FIFO, (1u << 16) | (1u << 4)); /* packet id 1, protocol I2C */
    wr(I2C5 + I2C_TX_FIFO, 0);                      /* one byte */
    wr(I2C5 + I2C_TX_FIFO, (1u << 17) | (repeat_start ? 1u << 16 : 0) | (read ? 1u << 19 : 0) | (address << 1));
    if (!read)
        wr(I2C5 + I2C_TX_FIFO, out & 0xff);
    const uint64_t end = now() + 5000 * per_us;
    int got = !read, done = 0;
    while (!done) {
        const uint32_t status = rd(I2C5 + I2C_INT_STATUS);
        if (read && !got && (rd(I2C5 + I2C_FIFO_STATUS) & 0xf)) {
            *in = rd(I2C5 + I2C_RX_FIFO);
            got = 1;
        }
        if (status & (INT_NO_ACK | INT_ARB_LOST))
            break;
        if (status & INT_PACKET_DONE) {
            if (read && !got && (rd(I2C5 + I2C_FIFO_STATUS) & 0xf)) {
                *in = rd(I2C5 + I2C_RX_FIFO);
                got = 1;
            }
            done = 1;
        } else if (now() > end) {
            break;
        }
    }
    wr(I2C5 + I2C_INT_STATUS, rd(I2C5 + I2C_INT_STATUS));
    return !(done && got);
}

/* VDD_CORE from the MAX77620's SD0, over I2C5 as U-Boot left it clocked; 0 when it cannot be read. */
static uint32_t core_mv(void)
{
    if (!(rd(CAR + 0x014) & (1u << 15)) || (rd(CAR + 0x008) & (1u << 15)))
        return 0; /* I2C5 ungated, out of reset */
    const uint32_t cnfg = rd(I2C5 + I2C_CNFG), want = cnfg | CNFG_PACKET_MODE | CNFG_NEW_MASTER_FSM;
    if (cnfg != want) {
        wr(I2C5 + I2C_CNFG, want);
        wr(I2C5 + I2C_CONFIG_LOAD, 1);
        if (wait_for(I2C5 + I2C_CONFIG_LOAD, 1, 0, 2000))
            return 0;
    }
    wr(I2C5 + I2C_INT_MASK, 0);
    uint32_t word = 0;
    if (i2c_packet(MAX77620, 0, MAX77620_REG_SD0, 0, 1) || i2c_packet(MAX77620, 1, 0, &word, 0))
        return 0;
    return 600 + (word & 0x7f) * 25 / 2;
}

static int64_t rate_of(const uint32_t *words)
{
    return (int64_t)((uint64_t)words[1] << 32 | words[0]);
}

static uint32_t emc_rate(uint64_t area, uint32_t cmd, int64_t hz, int64_t *rate, int32_t *code)
{
    const uint32_t in[4] = {CLK(cmd), 0, (uint32_t)hz, (uint32_t)((uint64_t)hz >> 32)};
    uint32_t out[2] = {0, 0};
    const uint32_t failure = bpmp_call(area, MRQ_CLK, in, cmd == CMD_CLK_GET_RATE ? 1 : 4, out, 2,
                                       cmd == CMD_CLK_SET_RATE ? 1000000 : 100000, code);
    if (failure)
        return failure;
    *rate = rate_of(out);
    return *code || *rate <= 0 ? LOADER_EMC_REFUSED : 0;
}

void loader_emc_raise(uint32_t khz, uint32_t frequency, struct loader_emc_report *report)
{
    report->result = LOADER_EMC_RAISED;
    report->bpmp_code = 0;
    report->before_khz = report->after_khz = report->core_mv = report->area = 0;
    per_us = frequency / 1000000;
    if (((rd(HIDREV) >> 4) & 0xfu) != 2u) {
        report->result = LOADER_EMC_NOT_T210B01;
        return;
    }
    if (!rd(SEMA)) {
        report->result = LOADER_EMC_NO_FIRMWARE;
        return;
    }
    wr(ATOMICS, TRIGGER_CMD_GET); /* channel 0 << 16 */
    const uint32_t area = rd(ATOMICS + 0xc00);
    report->area = area;
    /* Where BPMP firmware keeps channel areas: IRAM, or its carveout at the top of RAM (outside XNU's RAM). */
    if (!area || (area & 3) ||
        !((area >= 0x40000000u && area <= 0x4003ff80u) || (area >= 0xffd00000u && area <= 0xffffff80u))) {
        report->result = LOADER_EMC_CHANNEL;
        return;
    }
    uint32_t words[DATA_WORDS];
    int32_t code = 0;
    uint32_t failure;
    const uint32_t challenge = 0x10101010;
    failure = bpmp_call(area, MRQ_PING, &challenge, 1, words, 1, 100000, &code);
    if (!failure && (code || words[0] != challenge << 1))
        failure = LOADER_EMC_PING;
    if (!failure) {
        failure = bpmp_call(area, MRQ_EMC_DVFS_TABLE, 0, 0, words, 29, 100000, &code);
        unsigned found = 0;
        for (unsigned i = 0; !failure && !code && i < words[0] && i < 14; ++i)
            found |= words[1 + 2 * i] == khz;
        if (!failure && (code || !words[0] || words[0] > 14 || !found))
            failure = LOADER_EMC_TABLE;
    }
    if (!failure) {
        const uint32_t info = CLK(CMD_CLK_GET_ALL_INFO);
        failure = bpmp_call(area, MRQ_CLK, &info, 1, words, 29, 100000, &code);
        const char name[4] = {'e', 'm', 'c', 0};
        for (unsigned i = 0; !failure && i < 4; ++i)
            if (((words[(CLK_NAME_OFFSET + i) / 4] >> (8 * ((CLK_NAME_OFFSET + i) % 4))) & 0xff) != (uint32_t)name[i])
                failure = LOADER_EMC_NOT_EMC;
        if (!failure && code)
            failure = LOADER_EMC_NOT_EMC;
    }
    int64_t before = 0, rounded = 0, after = 0;
    if (!failure && emc_rate(area, CMD_CLK_GET_RATE, 0, &before, &code))
        failure = LOADER_EMC_NOT_EMC;
    report->before_khz = (uint32_t)(before / 1000);
    if (!failure) {
        uint32_t need = 0;
        for (unsigned i = 0; i < sizeof(emc_core_mv) / sizeof(emc_core_mv[0]) && !need; ++i)
            if (khz <= emc_core_mv[i][0])
                need = emc_core_mv[i][1];
        report->core_mv = core_mv();
        if (!report->core_mv)
            failure = LOADER_EMC_I2C;
        else if (!need || report->core_mv < need)
            failure = LOADER_EMC_LOW_CORE;
    }
    if (!failure) {
        const uint32_t enable = CLK(CMD_CLK_ENABLE);
        failure = bpmp_call(area, MRQ_CLK, &enable, 1, words, 0, 100000, &code);
        if (!failure && code)
            failure = LOADER_EMC_REFUSED;
    }
    if (!failure)
        failure = emc_rate(area, CMD_CLK_ROUND_RATE, (int64_t)khz * 1000, &rounded, &code);
    if (!failure && rounded != before)
        failure = emc_rate(area, CMD_CLK_SET_RATE, rounded, &after, &code);
    if (!failure)
        failure = emc_rate(area, CMD_CLK_GET_RATE, 0, &after, &code);
    report->after_khz = (uint32_t)(after / 1000);
    report->result = failure;
    report->bpmp_code = code;
}
