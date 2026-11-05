// Host harness for drivers/SwitchBluetooth/SwitchBluetoothRxDma.h (tests/test_switch_bluetooth_rx_dma.py): the header's
// static inline framing and transfer-length functions as exported ones for ctypes, on one frame of the header's struct.
#include <string.h>

#include "SwitchBluetoothRxDma.h"

static struct switch_bt_h4_frame frame;

void bt_frame_reset(void)
{
    memset(&frame, 0, sizeof(frame));
}

void bt_frame_lose(void)
{
    frame.lost = true;
}

int bt_frame_byte(uint8_t byte)
{
    return switch_bt_h4_frame_byte(&frame, byte);
}

int bt_frame_left(uint32_t *left)
{
    return switch_bt_h4_frame_left(&frame, left);
}

uint32_t bt_rx_dma_length(void)
{
    return switch_bt_rx_dma_length(&frame);
}

uint32_t bt_rx_dma_visible(uint32_t length, uint32_t moved_bytes, int ended, uint32_t taken)
{
    return switch_bt_rx_dma_visible(length, moved_bytes, ended != 0, taken);
}

uint32_t bt_rx_dma_reach(void)
{
    return SWITCH_BT_RX_DMA_REACH;
}

uint32_t bt_rx_dma_words(void)
{
    return SWITCH_BT_RX_DMA_WORDS;
}
