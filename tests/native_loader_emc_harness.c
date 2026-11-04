/* native/loader/emc.c against a model of what it talks to: the T210 BPMP mailbox (arbitration semaphore, atomics
 * trigger, channel 0's shared area, the outbound doorbell) with firmware answering like hekate's bpmpfw_b01.bin, the
 * I2C5 packet interface with a MAX77620 behind it, CAR's I2C5 clock bits, and a counter that advances on every read.
 * Every access emc.c makes goes through rd/wr below; anything outside the model aborts the test. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emc.h"

#define SEMA 0x60001000u
#define ATOMICS 0x70016000u
#define FIR_SET 0x60004018u
#define CAR 0x60006000u
#define I2C5 0x7000d000u
#define AREA_IRAM 0x4003f000u

static struct {
    uint32_t sta, area, area_mem[32], trigger;
    unsigned rings, requests, set_rates, frees;
    int answer, firmware, dead_after;
    uint64_t emc_hz;
    uint32_t table_khz[4];
    unsigned table_n;
    char clock_name[8];
    int32_t refuse_set;
    /* I2C5 */
    uint32_t cnfg, int_status, fifo_ctl, rx_word, rx_ready, tx[8], tx_n, sd0, i2c_ok, clock_on, reg_selected, int_mask;
    uint64_t counter;
} m;

uint64_t now(void) { return m.counter += 19; } /* one microsecond per look */

static void ack(void) { m.sta = (m.sta & ~3u) | 3u; } /* the firmware's bpmp_ack_master with DO_ACK */

static void bpmp_answer(void) {
    uint32_t *a = m.area_mem;
    const uint32_t mrq = a[0];
    uint32_t *d = &a[2];
    ++m.requests;
    assert(a[1] == 1u); /* DO_ACK, no RING_DOORBELL: the firmware must not interrupt the CPU */
    if (m.dead_after >= 0 && (int)m.requests > m.dead_after) return; /* never ACKs */
    a[0] = 0;
    if (mrq == 0) {
        d[0] = d[0] << 1;
    } else if (mrq == 32) {
        d[0] = m.table_n;
        for (unsigned i = 0; i < m.table_n; ++i) { d[1 + 2 * i] = m.table_khz[i]; d[2 + 2 * i] = 1100; }
    } else if (mrq == 22) {
        const uint32_t cmd = d[0] >> 24, id = d[0] & 0xffffff;
        assert(id == 0);
        if (cmd == 14) {
            memset(d, 0, 120);
            memcpy((uint8_t *)d + 73, m.clock_name, strlen(m.clock_name) + 1);
        } else if (cmd == 1) {
            d[0] = (uint32_t)m.emc_hz; d[1] = (uint32_t)(m.emc_hz >> 32);
        } else if (cmd == 3 || cmd == 2) {
            const uint64_t hz = (uint64_t)d[3] << 32 | d[2];
            if (cmd == 2) {
                ++m.set_rates;
                if (m.refuse_set) a[0] = (uint32_t)m.refuse_set;
                else m.emc_hz = hz;
            }
            d[0] = (uint32_t)hz; d[1] = (uint32_t)(hz >> 32);
        } else if (cmd != 7) {
            abort();
        }
    } else {
        abort();
    }
    ack();
}

static void i2c_run(void) {
    /* header words: [0] packet id/protocol, [1] length - 1, [2] I2C header, then data for a write */
    assert(m.tx[0] == ((1u << 16) | (1u << 4)) && m.tx[1] == 0);
    const uint32_t header = m.tx[2], address = (header >> 1) & 0x7f;
    if (!m.i2c_ok || address != 0x3c) { m.int_status |= 1u << 3; return; } /* NO_ACK */
    if (header & (1u << 19)) {
        assert(m.reg_selected == 0x16);
        m.rx_word = m.sd0;
        m.rx_ready = 1;
    } else {
        if (m.tx_n < 4) return; /* data word still to come */
        m.reg_selected = m.tx[3] & 0xff;
        assert(header & (1u << 16)); /* repeat start before the read */
    }
    m.int_status |= 1u << 7; /* PACKET_DONE */
}

uint32_t rd(uint64_t address) {
    if (address == SEMA) return m.firmware ? m.sta | 0x10000u : 0; /* other channels' bits non-zero: handshake */
    if (address == ATOMICS + 0xc00) return m.area;
    if (address >= m.area && address < m.area + 128) return m.area_mem[(address - m.area) / 4];
    if (address == CAR + 0x014) return m.clock_on ? 1u << 15 : 0;
    if (address == CAR + 0x008) return 0;
    if (address == I2C5 + 0x00) return m.cnfg;
    if (address == I2C5 + 0x5c) return m.fifo_ctl;
    if (address == I2C5 + 0x60) return m.rx_ready ? 1u : 0;
    if (address == I2C5 + 0x54) { m.rx_ready = 0; return m.rx_word; }
    if (address == I2C5 + 0x68) return m.int_status;
    if (address == I2C5 + 0x8c) return 0;
    fprintf(stderr, "read outside the model: %#llx\n", (unsigned long long)address);
    abort();
}

void wr(uint64_t address, uint32_t value) {
    if (address == SEMA + 8) {
        if (value == 3u) { /* signal slave: free -> signalled; the firmware answers at once */
            assert((m.sta & 3u) == 2u);
            m.sta &= ~3u;
            bpmp_answer();
        } else if (value == 1u) { /* free master: acked -> free */
            assert((m.sta & 3u) == 3u);
            m.sta = (m.sta & ~3u) | 2u;
            ++m.frees;
        } else {
            abort();
        }
        return;
    }
    if (address == ATOMICS) { assert(value == 4u); m.trigger = value; return; }
    if (address == FIR_SET) { assert(value == 1u << 6); ++m.rings; return; }
    if (address >= m.area && address < m.area + 128) { m.area_mem[(address - m.area) / 4] = value; return; }
    if (address == I2C5 + 0x00) { m.cnfg = value; return; }
    if (address == I2C5 + 0x8c) return;
    if (address == I2C5 + 0x64) { m.int_mask = value; return; }
    if (address == I2C5 + 0x68) { m.int_status &= ~value; return; }
    if (address == I2C5 + 0x5c) { m.fifo_ctl = value & ~3u; m.tx_n = 0; m.rx_ready = 0; return; }
    if (address == I2C5 + 0x50) {
        assert(m.tx_n < 8);
        m.tx[m.tx_n++] = value;
        if (m.tx_n >= 3) i2c_run();
        return;
    }
    fprintf(stderr, "write outside the model: %#llx = %#x\n", (unsigned long long)address, value);
    abort();
}

static void reset(void) {
    memset(&m, 0, sizeof(m));
    m.firmware = 1;
    m.sta = 2; /* channel 0 free */
    m.area = AREA_IRAM;
    m.dead_after = -1;
    m.emc_hz = 204000000;
    m.table_n = 4;
    m.table_khz[0] = 204000; m.table_khz[1] = 1331200; m.table_khz[2] = 1600000; m.table_khz[3] = 2133000;
    strcpy(m.clock_name, "emc");
    m.sd0 = 0x24; /* 600 + 36 * 12.5 = 1050 mV, as hekate leaves it on T210B01 */
    m.i2c_ok = 1;
    m.clock_on = 1;
    m.cnfg = 0;
}

int main(void) {
    struct loader_emc_report r;
    unsigned checks = 0;

    reset();
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_RAISED && r.before_khz == 204000 && r.after_khz == 1600000 && r.core_mv == 1050);
    assert(m.emc_hz == 1600000000ull && m.set_rates == 1 && m.rings == m.requests && m.frees == m.requests);
    assert((m.sta & 3u) == 2u && (m.cnfg & ((1u << 10) | (1u << 11))) == ((1u << 10) | (1u << 11)));
    ++checks;

    /* Already at the rate (the kernel's later call, or a second run): no SET_RATE. */
    m.set_rates = 0;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_RAISED && m.set_rates == 0 && r.before_khz == 1600000);
    ++checks;

    /* Every refusal leaves the clock where it was and the channel free (or untouched). */
    reset(); m.firmware = 0;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_NO_FIRMWARE && m.requests == 0); ++checks;

    reset(); m.area = 0x80000000u;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_CHANNEL && m.requests == 0 && r.area == 0x80000000u); ++checks;

    reset(); m.sta = 0; /* never free */
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_BUSY && m.requests == 0); ++checks;

    reset(); m.dead_after = 0;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_NO_ANSWER && m.requests == 1 && m.emc_hz == 204000000); ++checks;

    reset(); m.dead_after = 5; /* the firmware stops answering mid-sequence (at ROUND_RATE) */
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_NO_ANSWER && m.set_rates == 0 && m.emc_hz == 204000000); ++checks;

    reset(); m.table_n = 3; m.table_khz[2] = 2133000;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_TABLE && m.set_rates == 0); ++checks;

    reset(); strcpy(m.clock_name, "sclk");
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_NOT_EMC && m.set_rates == 0); ++checks;

    reset(); m.i2c_ok = 0;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_I2C && m.set_rates == 0 && m.emc_hz == 204000000); ++checks;

    reset(); m.clock_on = 0;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_I2C && m.set_rates == 0); ++checks;

    reset(); m.sd0 = 0x05; /* 662 mV < 675 */
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_LOW_CORE && r.core_mv == 662 && m.set_rates == 0); ++checks;

    reset(); m.refuse_set = -22;
    loader_emc_raise(1600000, 19200000, &r);
    assert(r.result == LOADER_EMC_REFUSED && r.bpmp_code == -22 && m.emc_hz == 204000000 && (m.sta & 3u) == 2u); ++checks;

    printf("PASS emc %u scenarios\n", checks);
    return 0;
}
