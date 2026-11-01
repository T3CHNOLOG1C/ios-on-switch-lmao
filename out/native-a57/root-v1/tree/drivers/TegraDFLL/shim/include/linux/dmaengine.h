/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: the dmaengine types drivers/soc/tegra/fuse/fuse.h names for Tegra20 fuse reads (unused on T210). */
#ifndef TDFLL_LINUX_DMAENGINE_H
#define TDFLL_LINUX_DMAENGINE_H
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/completion.h>
struct dma_chan;
struct dma_slave_config {
	int direction;
	phys_addr_t src_addr;
	phys_addr_t dst_addr;
};
#endif
