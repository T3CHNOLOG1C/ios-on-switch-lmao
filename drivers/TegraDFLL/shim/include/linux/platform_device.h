/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IOSwitch shim: platform devices and drivers. There is no driver core: platform.c builds the one platform_device
 * (the DFLL at 0x70110000, resources from the DT reg property) and dfll.c calls the vendored driver's probe through
 * builtin_platform_driver(), which here only exports the driver structure instead of registering an initcall.
 */
#ifndef TDFLL_LINUX_PLATFORM_DEVICE_H
#define TDFLL_LINUX_PLATFORM_DEVICE_H
#include <linux/device.h>
#include <linux/of.h>

#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IRQ 0x00000400
struct resource {
	resource_size_t start;
	resource_size_t end;
	const char *name;
	unsigned long flags;
};
static inline resource_size_t resource_size(const struct resource *res) { return res->end - res->start + 1; }

struct platform_device {
	const char *name;
	int id;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
};
#define to_platform_device(x) container_of((x), struct platform_device, dev)

struct platform_driver {
	int (*probe)(struct platform_device *);
	int (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};

struct resource *platform_get_resource(struct platform_device *pdev, unsigned int type, unsigned int num);
static inline void *platform_get_drvdata(const struct platform_device *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device *pdev, void *data) { dev_set_drvdata(&pdev->dev, data); }

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size);
void __iomem *devm_ioremap_resource(struct device *dev, struct resource *res);

/* The vendored file's driver, for dfll.c (one per link: clk-tegra124-dfll-fcpu.c's). */
struct platform_driver *tdfll_builtin_platform_driver(void);
#define builtin_platform_driver(__platform_driver) \
	struct platform_driver *tdfll_builtin_platform_driver(void) { return &(__platform_driver); } \
	_Static_assert(1, "builtin_platform_driver")
#endif
