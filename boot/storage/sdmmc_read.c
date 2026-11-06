#include "sdmmc_read.h"

/* SDHCI register layout cross-checked against the pinned Switch U-Boot
 * tegra_mmc.h/tegra_mmc.c. This is a new PIO implementation; U-Boot uses DMA.
 * No PHY, power, clock, tuning, GPIO, DMA-address or reset writes occur. */
enum {
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    BUFFER = 0x20,
    PRESENT = 0x24,
    HOST_CONTROL = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    RESET = 0x2f,
    STATUS = 0x30,
    STATUS_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
    VERSION = 0xfe,
    COMMAND_DONE = 1,
    TRANSFER_DONE = 2,
    BUFFER_READY = 0x20,
    ERRORS = 0xffff8000u,
    OWN_EVENTS = 0x23,
    ENABLE_EVENTS = 0x03ff8023,
    /* R1: error bits only; READY_FOR_DATA/current state are not errors. */
    CARD_ERRORS = 0xfdffe008u,
    MAX_POLLS = 1000000
};

static int wait_event(struct switch_sd_host *h, uint32_t event, struct switch_sd_result *r)
{
    uint64_t started = h->io.ticks(h->context);
    for (uint32_t polls = 0; polls < MAX_POLLS; ++polls) {
        r->status = h->io.read32(h->context, STATUS);
        if (r->status & ERRORS)
            return SWITCH_SD_CONTROLLER_ERROR;
        if (r->status & event)
            return SWITCH_SD_OK;
        /* One second, bounded also when a broken counter does not advance. */
        if (h->io.ticks(h->context) - started >= h->frequency)
            return SWITCH_SD_TIMEOUT;
    }
    return SWITCH_SD_TIMEOUT;
}

int switch_sd_read_sector(struct switch_sd_host *h, uint64_t lba, uint8_t output[512], struct switch_sd_result *r)
{
    if (!h || !r || !output || !h->io.read8 || !h->io.read16 || !h->io.read32 || !h->io.write16 || !h->io.write32 ||
        !h->io.ticks || !h->io.barrier)
        return SWITCH_SD_ARGUMENT;
    *r = (struct switch_sd_result){0};
    if (h->blocked)
        return SWITCH_SD_ALREADY_BLOCKED;
    if (h->block_addressing_verified > 1 ||
        (!h->block_addressing_verified &&
         (h->permitted_first_sector || h->permitted_sector_count != 1 || h->capacity_sectors != 1)) ||
        h->frequency < 1000 || h->frequency > 1000000000 || !h->capacity_sectors ||
        h->capacity_sectors > UINT64_C(0x100000000) || h->permitted_first_sector >= h->capacity_sectors ||
        !h->permitted_sector_count || h->permitted_sector_count > h->capacity_sectors - h->permitted_first_sector)
        return SWITCH_SD_UNQUALIFIED;
    if (lba < h->permitted_first_sector || lba - h->permitted_first_sector >= h->permitted_sector_count)
        return SWITCH_SD_ARGUMENT;
    r->stage = 1;
    /* Sample every readiness register exactly once. A later diagnostic read
     * may differ from the value which caused a refusal. Preserve all samples
     * and all failed predicates, including when an earlier predicate fails. */
    r->present = h->io.read32(h->context, PRESENT);
    r->version = h->io.read16(h->context, VERSION);
    r->host_control = h->io.read8(h->context, HOST_CONTROL);
    r->power = h->io.read8(h->context, POWER);
    r->clock = h->io.read16(h->context, CLOCK);
    r->reset = h->io.read8(h->context, RESET);
    r->block_size = h->io.read16(h->context, BLOCK_SIZE);
    /* SDHCI 3.0/4.0 retain this single-block PIO path: nonzero 16-bit block
     * count, no DMA, no Auto CMD. Accept only these reviewed revisions.
     * Inserted and stable, command/data idle; inherited 4-bit bus. */
    uint32_t spec_version = r->version & 0xff;
    if (spec_version != 2 && spec_version != 3)
        r->readiness_failures |= SWITCH_SD_READY_VERSION;
    if (r->present & 0x307)
        r->readiness_failures |= SWITCH_SD_READY_ACTIVE;
    if ((r->present & 0x30000) != 0x30000)
        r->readiness_failures |= SWITCH_SD_READY_CARD;
    if ((r->host_control & 0x22) != 2)
        r->readiness_failures |= SWITCH_SD_READY_BUS;
    if (!(r->power & 1))
        r->readiness_failures |= SWITCH_SD_READY_POWER;
    if ((r->clock & 7) != 7)
        r->readiness_failures |= SWITCH_SD_READY_CLOCK;
    if (r->reset)
        r->readiness_failures |= SWITCH_SD_READY_RESET;
    if ((r->block_size & 0xfff) != 512)
        r->readiness_failures |= SWITCH_SD_READY_BLOCK_SIZE;
    if (r->readiness_failures)
        return SWITCH_SD_NOT_READY;
    r->status = h->io.read32(h->context, STATUS);
    r->initial_status = r->status;
    if (r->status & ERRORS)
        return SWITCH_SD_PREEXISTING_ERROR;
    uint32_t prior_enable = h->io.read32(h->context, STATUS_ENABLE);
    uint32_t prior_signal = h->io.read32(h->context, SIGNAL_ENABLE);
    uint16_t prior_size = (uint16_t)r->block_size;
    uint16_t prior_count = h->io.read16(h->context, BLOCK_COUNT);
    uint16_t prior_transfer = h->io.read16(h->context, TRANSFER);
    uint32_t prior_argument = h->io.read32(h->context, ARGUMENT);
    uint8_t sector[512];
    h->io.write32(h->context, SIGNAL_ENABLE, 0);
    h->io.write32(h->context, STATUS_ENABLE, prior_enable | ENABLE_EVENTS);
    h->io.write32(h->context, STATUS, OWN_EVENTS);
    h->io.write16(h->context, BLOCK_SIZE, 512);
    h->io.write16(h->context, BLOCK_COUNT, 1);
    h->io.write32(h->context, ARGUMENT, (uint32_t)lba);
    /* Single-block read, no DMA or automatic commands. */
    h->io.write16(h->context, TRANSFER, 0x12);
    h->io.barrier(h->context);
    /* CMD17, 48-bit R1, index+CRC checked, data present. */
    h->io.write16(h->context, COMMAND, 0x113a);
    r->stage = 2;
    int error = wait_event(h, COMMAND_DONE, r);
    if (error)
        goto failed;
    r->response = h->io.read32(h->context, RESPONSE);
    if (r->response & CARD_ERRORS) {
        error = SWITCH_SD_CARD_ERROR;
        goto failed;
    }
    h->io.write32(h->context, STATUS, COMMAND_DONE);
    r->stage = 3;
    error = wait_event(h, BUFFER_READY, r);
    if (error)
        goto failed;
    if (!(h->io.read32(h->context, PRESENT) & 0x800)) {
        error = SWITCH_SD_BUFFER_STATE;
        goto failed;
    }
    for (uint32_t i = 0; i < 128; ++i) {
        uint32_t word = h->io.read32(h->context, BUFFER);
        for (uint32_t j = 0; j < 4; ++j)
            sector[i * 4 + j] = (uint8_t)(word >> (j * 8));
        r->words_read = i + 1;
    }
    h->io.write32(h->context, STATUS, BUFFER_READY);
    r->stage = 4;
    error = wait_event(h, TRANSFER_DONE, r);
    if (error)
        goto failed;
    h->io.write32(h->context, STATUS, TRANSFER_DONE);
    h->io.barrier(h->context);
    if (h->io.read32(h->context, PRESENT) & 0x307) {
        error = SWITCH_SD_BUFFER_STATE;
        goto failed;
    }
    h->io.write16(h->context, BLOCK_SIZE, prior_size);
    h->io.write16(h->context, BLOCK_COUNT, prior_count);
    h->io.write16(h->context, TRANSFER, prior_transfer);
    h->io.write32(h->context, ARGUMENT, prior_argument);
    h->io.write32(h->context, STATUS_ENABLE, prior_enable);
    h->io.write32(h->context, SIGNAL_ENABLE, prior_signal);
    h->io.barrier(h->context);
    for (uint32_t i = 0; i < 512; ++i)
        output[i] = sector[i];
    r->stage = 5;
    return SWITCH_SD_OK;
failed:
    h->blocked = 1;
    /* Keep signals masked. In-flight hardware is not silently returned to a
     * previous owner; caller must stop storage use and report this failure. */
    return error;
}
