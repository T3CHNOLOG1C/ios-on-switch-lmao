// SPDX-License-Identifier: GPL-2.0-only
#ifndef IOSW_INSTALL_STORAGE_H
#define IOSW_INSTALL_STORAGE_H
#include <stdint.h>
int installer_card_bind(void);
int installer_card_ready(void *);
int installer_card_sync(void *);
int installer_card_qualify(void);
int installer_card_transfer(void *, int write, uint64_t sector, uint32_t count, void *);
void installer_card_permit(int (*permit)(uint64_t, uint32_t));
void installer_card_offline(void);
#endif
