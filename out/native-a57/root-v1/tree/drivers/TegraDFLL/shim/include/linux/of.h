/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IOSwitch shim: a read-only device tree of static nodes (platform.c holds the DFLL node with the properties the
 * switchroot OLED DT gives it). Properties are stored decoded: u32 cells, strings, or presence-only booleans.
 */
#ifndef TDFLL_LINUX_OF_H
#define TDFLL_LINUX_OF_H
#include <linux/kernel.h>

struct tdfll_property {
	const char *name;
	const u32 *cells;           /* u32 values, or NULL */
	unsigned int ncells;
	const char *const *strings; /* string list, or NULL */
	unsigned int nstrings;
	struct device_node *phandle;  /* phandle-valued property target, or NULL */
};
struct device_node {
	const char *name;
	const char *full_name;
	const struct tdfll_property *properties;  /* terminated by a NULL name */
};
struct of_device_id {
	char name[32];
	char type[32];
	char compatible[128];
	const void *data;
};

int of_property_read_u32(const struct device_node *np, const char *propname, u32 *out_value);
int of_property_read_string(const struct device_node *np, const char *propname, const char **out_string);
bool of_property_read_bool(const struct device_node *np, const char *propname);
struct device_node *of_parse_phandle(const struct device_node *np, const char *phandle_name, int index);
static inline void of_node_put(struct device_node *node) { (void)node; }
static inline struct device_node *of_node_get(struct device_node *node) { return node; }
int of_device_is_compatible(const struct device_node *device, const char *compat);
bool of_device_is_available(const struct device_node *device);
const struct of_device_id *of_match_node(const struct of_device_id *matches, const struct device_node *node);
#endif
