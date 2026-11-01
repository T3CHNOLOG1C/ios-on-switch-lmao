#ifndef IOSWITCH_SDMMC_DMA_H
#define IOSWITCH_SDMMC_DMA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct iosw_sd_io;
struct iosw_sd_result;
#define IOSW_SD_DMA_BYTES 65536u
/* One private wired contiguous allocation, retained for the entire boot.
 * Physical is the qualified device bus address, never the CPU virtual address.
 * bus_ready proves the SDMMC1 MC/SMMU path still bypasses translation. cache
 * cleans+invalidates before DMA (after_read=0), invalidates after reads (1),
 * and completes with DSB. Each callback returns zero on success.
 * Caller serializes this state with every other use of the controller.
 * No buffer access/reuse/free is allowed after failed becomes nonzero: an
 * uncertain controller may still own it. No reset, retry or PIO fallback. */
struct iosw_sd_dma {
    uint8_t *bytes;
    uint64_t physical;
    uint32_t capacity;
    void (*write8)(void *, uint32_t, uint8_t);
    int (*bus_ready)(void *);
    int (*cache)(void *, void *, uint32_t, int after_read);
    uint64_t end, next_boundary, started, frequency;
    uint32_t bound, active, completed, failed, length, blocks, writing;
    uint32_t polls, boundaries;
    uint64_t read_bytes, write_bytes;
};
/* Qualify and enable Tegra's SDHCI v4 64-bit SDMA. No clock, power, voltage,
 * tuning, card command, SMMU or memory-controller register is changed. */
int iosw_sd_dma_bind(struct iosw_sd_dma *, const struct iosw_sd_io *, void *, uint64_t frequency);
/* Called after transfer/count/direction are configured, before the command.
 * Stages the COMPLETE write before any DMA address is submitted. */
int iosw_sd_dma_prepare(struct iosw_sd_dma *, const struct iosw_sd_io *, void *,
    uint32_t block_size, uint32_t count, const uint8_t *input);
/* Command R1 has already succeeded. Accepts only error-free full transfer;
 * handles exact 512 KiB SDMA boundaries, never restarting beyond the request. */
int iosw_sd_dma_wait(struct iosw_sd_dma *, const struct iosw_sd_io *, void *, struct iosw_sd_result *);
/* Call only after transfer/stop/idle checks succeed. Failed reads never copy. */
int iosw_sd_dma_complete(struct iosw_sd_dma *, void *, uint8_t *output);
void iosw_sd_dma_abort(struct iosw_sd_dma *);
#ifdef __cplusplus
}
#endif
#endif
