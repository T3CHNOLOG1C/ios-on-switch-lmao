/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: of_match_device() (best match by the node's compatible order, as Linux __of_match_node does). */
#ifndef TDFLL_LINUX_OF_DEVICE_H
#define TDFLL_LINUX_OF_DEVICE_H
#include <linux/device.h>
#include <linux/of.h>
static inline const struct of_device_id *of_match_device(const struct of_device_id *matches, const struct device *dev)
{
	if (!matches || !dev->of_node)
		return NULL;
	return of_match_node(matches, dev->of_node);
}
#endif
