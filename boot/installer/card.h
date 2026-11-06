// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_CARD_H
#define SWITCH_INSTALL_CARD_H

#include <stdint.h>

#include "target.h"

// Every SD write in the payload (FatFs's included) goes through the permit: none until a range is allowed.
void card_permit(uint64_t first, uint64_t sectors);
void card_permit_none(void);

int card_read(uint64_t sector, uint32_t count, void *buffer);
int card_write(uint64_t sector, uint32_t count, const void *buffer);

// Both GPT copies into `gpt`, checked (target.h).
int card_read_gpt(struct switch_gpt *gpt);

// `gpt` onto the card, backup copy first, each sector read back; only the four table ranges are permitted meanwhile.
int card_write_gpt(const struct switch_gpt *gpt);

#endif
