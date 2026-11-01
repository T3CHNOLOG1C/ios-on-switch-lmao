/* SPDX-License-Identifier: MIT */
/* IOSwitch: no debugfs on XNU. */
#ifndef NVXNU_LINUX_DEBUGFS_H
#define NVXNU_LINUX_DEBUGFS_H
struct dentry;
struct debugfs_blob_wrapper { void *data; unsigned long size; };
#define debugfs_create_dir(n, p) ((struct dentry *)NULL)
#define debugfs_create_blob(n, m, p, b) ((struct dentry *)NULL)
#define debugfs_remove(d) do { (void)(d); } while (0)
#define debugfs_remove_recursive(d) do { (void)(d); } while (0)
#endif
