// switch_bluetooth_rx_dma_harness.c's exports: SwitchBluetoothRxDma.h's H4 framing and transfer lengths on one frame,
// compiled as C.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bt_frame_reset(void);
void bt_frame_lose(void);
int bt_frame_byte(uint8_t byte);
int bt_frame_left(uint32_t *left);
uint32_t bt_rx_dma_length(void);
uint32_t bt_rx_dma_visible(uint32_t length, uint32_t moved_bytes, int ended, uint32_t taken);
uint32_t bt_rx_dma_reach(void);
uint32_t bt_rx_dma_words(void);

#ifdef __cplusplus
}
#endif
