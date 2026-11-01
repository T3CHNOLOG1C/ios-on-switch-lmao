/* Explicit raw profile only. The physical APFS store is writable; each
 * filesystem mount has its own policy. No change to the FAT boot profile. */
#include "IOSwitchFiles.h"
#include "../../drivers/NativeIOSwitchBlock/IOSwitchCryptex.h"
#include "storage/block_backend.h"
#include <sys/fcntl.h>

#define IOSW_RAW_DATA_UUID "D1B1D053-D0A6-496C-8625-A947A3DA863E"
#define IOSW_RAW_PREBOOT_UUID "4AA4A761-5138-439D-9A30-D4138F6887FA"
#define IOSW_RAW_BYTES UINT64_C(34359738368)
extern int ioswitch_raw_volume_status(const char *, dev_t *, char *, size_t, uint64_t *);
extern int native_owner_publish_raw_boot_manifest(dev_t, const char *, dev_t, const char *);

static int
native_ioswitch_raw_wait(const char *uuid, dev_t *device, char path[64])
{
	for (unsigned attempt = 0;; ++attempt) {
		uint64_t bytes = 0;
		int status = ioswitch_raw_volume_status(uuid, device, path, 64, &bytes);
		if (status == IOSW_BLOCK_OK) return bytes ? 0 : EINVAL;
		if (status != IOSW_BLOCK_NOT_READY) return status == IOSW_BLOCK_NOMEM ? ENOMEM :
		    status == IOSW_BLOCK_IO ? EIO : EINVAL;
		if (attempt == 100) return ETIMEDOUT;
		struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000};
		(void)msleep((void *)&native_owner_cache_stage, NULL, PWAIT, "ioswRaw", &interval);
	}
}

static int
native_ioswitch_raw_mount(proc_t p, user_addr_t scratch, const char *path,
    const char *device_path, dev_t expected, int readonly, mount_t *observed_mount)
{
	uint8_t payload[0x150] = {0};
	user64_addr_t spec = scratch + 0x160;
	uint64_t flags = readonly ? MNT_RDONLY : 0;
	uint16_t mode = 1;
	uint32_t anonymous_owner = 99;
	memcpy(payload, &spec, sizeof(spec));
	memcpy(payload + 8, &flags, sizeof(flags));
	memcpy(payload + 16, &mode, sizeof(mode));
	memcpy(payload + 0x144, &anonymous_owner, sizeof(anonymous_owner));
	memcpy(payload + 0x148, &anonymous_owner, sizeof(anonymous_owner));
	struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
	    .flags = (int)flags, .data = scratch + 0x200};
	int32_t result = 0;
	int error;
	if ((error = copyout("apfs", request.type, 5)) ||
	    (error = copyout(path, request.path, strlen(path) + 1)) ||
	    (error = copyout(device_path, spec, strlen(device_path) + 1)) ||
	    (error = copyout(payload, request.data, sizeof(payload)))) return error;
	error = mount(p, &request, &result);
	if (error) return error;
	vnode_t root = NULLVP;
	error = vnode_lookup(path, 0, &root, vfs_context_current());
	if (error) return error;
	mount_t mounted = vnode_mount(root);
	vnode_t device = vfs_devvp(mounted);
	dev_t actual = device ? vnode_specrdev(device) : NODEV;
	if (device) vnode_put(device);
	uint64_t actual_flags = vfs_flags(mounted);
	if (!vnode_isdir(root) || actual != expected ||
	    !!(actual_flags & MNT_RDONLY) != !!readonly ||
	    (actual_flags & (MNT_IGNORE_OWNERSHIP | MNT_UNION)) ||
	    strcmp(vfs_statfs(mounted)->f_fstypename, "apfs")) error = EPERM;
	if (!error && observed_mount) *observed_mount = mounted;
	vnode_put(root);
	return error;
}

static int
native_ioswitch_raw_directory(proc_t p, user_addr_t scratch, const char *path,
    mount_t expected, int create)
{
	vnode_t directory = NULLVP;
	int error = vnode_lookup(path, 0, &directory, vfs_context_current());
	if (error == ENOENT && create) {
		struct mkdir_args request = {.path = scratch + 0x900, .mode = 0755};
		int32_t result = 0;
		if ((error = copyout(path, request.path, strlen(path) + 1))) return error;
		if ((error = mkdir(p, &request, &result))) return error;
		error = vnode_lookup(path, 0, &directory, vfs_context_current());
	}
	if (error) return error;
	if (vnode_mount(directory) != expected || !vnode_isdir(directory)) error = ENOTDIR;
	vnode_put(directory);
	return error;
}

extern volatile int native_bindfs_boot_mount;

/* Read-only bindfs union of `source` over `point`: lookups try `source` first,
 * then fall through to the point's own entries. */
static int
native_ioswitch_raw_union(proc_t p, user_addr_t scratch, const char *source, const char *point)
{
	struct mount_args request = {.type = scratch + 0xa00, .path = scratch + 0xa20,
	    .flags = MNT_RDONLY | MNT_UNION, .data = scratch + 0xb00};
	int32_t result = 0;
	int error;
	if ((error = copyout("bindfs", request.type, 7)) ||
	    (error = copyout(point, request.path, strlen(point) + 1)) ||
	    (error = copyout(source, request.data, strlen(source) + 1))) return error;
	native_bindfs_boot_mount = 1;
	error = mount(p, &request, &result);
	native_bindfs_boot_mount = 0;
	return error;
}

#define IOSW_RAW_STEP(step, call) do { error = (call); \
	if (error) { printf("IOSwitch raw boot: %s failed: %d\n", (step), error); return error; } } while (0)

/* Mount Preboot read-write, attach and mount the original System/App cryptex
 * images from it, and point launchd's dyld at the System cryptex cache. */
static int
native_ioswitch_raw_cache_prepare(proc_t p, user_addr_t scratch, user_addr_t *envp)
{
	vnode_t original_root = rootvnode;
	mount_t os_mount = vnode_mount(original_root);
	vnode_t device = vfs_devvp(os_mount);
	dev_t os_device = device ? vnode_specrdev(device) : NODEV;
	if (device) vnode_put(device);
	int error = 0;
	if (!(vfs_flags(os_mount) & MNT_RDONLY) || !native_verified_block_root_authorized(os_device)) {
		printf("IOSwitch raw boot: root is not the read-only SD OS volume\n");
		return EAUTH;
	}
	dev_t preboot_device = NODEV;
	char path[64];
	mount_t preboot_mount = NULL;
	IOSW_RAW_STEP("Preboot volume lookup", native_ioswitch_raw_wait(IOSW_RAW_PREBOOT_UUID, &preboot_device, path));
	IOSW_RAW_STEP("/private/preboot lookup", native_ioswitch_raw_directory(p, scratch, "/private/preboot", os_mount, 0));
	IOSW_RAW_STEP("Preboot mount", native_ioswitch_raw_mount(p, scratch, "/private/preboot",
	    path, preboot_device, 0, &preboot_mount));
	static const char *const directories[] = {"/private/preboot/Cryptexes",
	    "/private/preboot/Cryptexes/OS", "/private/preboot/Cryptexes/App"};
	for (unsigned i = 0; i < 3; ++i)
		IOSW_RAW_STEP(directories[i], native_ioswitch_raw_directory(p, scratch, directories[i], preboot_mount, 1));
	dev_t cache_device = NODEV;
	for (unsigned i = 0; i < 2; ++i) {
		dev_t image_volume = NODEV;
		IOSW_RAW_STEP(i ? "App cryptex attach" : "System cryptex attach",
		    ioswitch_cryptex_attach(i, preboot_device, &image_volume, path, sizeof(path)));
		IOSW_RAW_STEP(i ? "App cryptex mount" : "System cryptex mount",
		    native_ioswitch_raw_mount(p, scratch, directories[i + 1], path, image_volume, 1, NULL));
		if (!i) cache_device = image_volume;
	}
	/* dyld looks for code that only the OS cryptex has under the cryptex prefix that
	 * libignition reports. Ignition is off here, so show each directory holding such
	 * code through the root's own (21H461: these are all of them). */
	static const char *const cryptex_only[] = {"/usr/lib",   // libobjc-trampolines, libffi-trampolines, ...
	    "/System/Library/PrivateFrameworks/PlugInKitDaemon.framework",   // pkd's framework, not in the cache
	    "/System/Library/PrivateFrameworks/VisualTestKit.framework",
	    "/System/DriverKit/System/Library"};   // the DriverKit shared cache
	for (unsigned i = 0; i < sizeof(cryptex_only) / sizeof(cryptex_only[0]); ++i) {
		char source[128];
		snprintf(source, sizeof(source), "/private/preboot/Cryptexes/OS%s", cryptex_only[i]);
		if ((error = native_ioswitch_raw_union(p, scratch, source, cryptex_only[i])))
			printf("IOSwitch raw boot: %s cryptex union failed: %d\n", cryptex_only[i], error);
	}
	if (!native_owner_publish_raw_boot_manifest(os_device,
	    "04769D26-2CA0-4825-B443-E2E5F88300F2", cache_device,
	    "8846FE78-2910-4297-AF97-D59BB6F8FC16"))
		printf("IOSwitch raw boot: boot-manifest-hash not published (continuing)\n");
	/* Same development dyld mode as the FAT profile; no signature-policy change. */
	dyld_flags |= UINT64_C(8);
	commpage_update_dyld_flags(dyld_flags);
	native_owner_cache_dyld_flags = dyld_flags;
	/* CacheFinder's existing override opens this exact mounted cryptex path. */
	static const char cache_environment[] =
	    "DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld";
	user64_addr_t environment_vector[2] = {scratch + 0x400, USER_ADDR_NULL};
	if ((error = copyout(cache_environment, environment_vector[0], sizeof(cache_environment))) ||
	    (error = copyout(environment_vector, scratch + 0x500, sizeof(environment_vector)))) return error;
	*envp = scratch + 0x500;
	native_owner_cache_selected_device = native_owner_cache_mounted_device = cache_device;
	native_owner_cache_readonly = native_owner_cache_authorized = native_owner_cache_root_recheck = 1;
	native_owner_cache_original_root = native_owner_cache_current_root = (uintptr_t)original_root;
	native_owner_cache_native_union = 0;
	native_owner_cache_stage = 6;
	native_owner_cache_status = native_owner_cache_mount_status = 0;
	return 0;
}

/* Userland overlay (tree/overlay/manifest.json, scripts/package_raw_boot.py): files the owner keeps on Data under
 * /private/var/ioswitch/overlay<absolute path>, shown read-only over sealed system directories. ADT /chosen
 * "ioswitch-overlay" lists NUL-terminated "<directory>[:<boot-arg>[|<boot-arg>...]]"; every listed directory with no
 * boot-arg, or with any of its boot-args non-zero (features sharing a directory each name their own), gets the same
 * read-only bindfs union as the cryptex directories above, so lookups find the
 * owner's file first and everything else falls through to the sealed entries. Called after the Data mount and
 * before launchd starts. The union reports its files as root:wheel (BINDM_FORCE_ROOT: the owner copies them
 * without root); code in them still needs its CDHash in the trust cache, which the packager adds as a module of its
 * own. A directory with nothing deployed is skipped; a failed union is logged and boot continues. */
#include <pexpert/device_tree.h>
static void
native_ioswitch_raw_overlay(proc_t p, user_addr_t scratch)
{
	DTEntry chosen;
	const char *list = NULL;
	unsigned int size = 0;
	if (SecureDTLookupEntry(0, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "ioswitch-overlay", (const void **)&list, &size) != kSuccess) return;
	unsigned shown = 0;
	for (unsigned at = 0; at < size;) {
		const size_t length = strnlen(list + at, size - at);
		char directory[128], source[176];
		if (!length || length >= sizeof(directory) || list[at] != '/') {
			at += (unsigned)length + 1;
			continue;
		}
		memcpy(directory, list + at, length);
		directory[length] = '\0';
		at += (unsigned)length + 1;
		char *gate = strchr(directory, ':');
		if (gate) {
			bool on = false;
			*gate++ = '\0';
			for (char *name = gate; name && !on;) {
				char *next = strchr(name, '|');
				uint32_t value = 0;
				if (next) *next++ = '\0';
				on = PE_parse_boot_argn(name, &value, sizeof(value)) && value;
				name = next;
			}
			if (!on) continue;
		}
		snprintf(source, sizeof(source), "/private/var/ioswitch/overlay%s", directory);
		vnode_t probe = NULLVP;
		int error = vnode_lookup(source, 0, &probe, vfs_context_current());
		if (error == ENOENT) continue;
		if (!error) {
			if (!vnode_isdir(probe)) error = ENOTDIR;
			vnode_put(probe);
		}
		if (!error) error = native_ioswitch_raw_union(p, scratch, source, directory);
		if (error) printf("IOSwitch overlay: %s over %s failed: %d\n", source, directory, error);
		else ++shown;
	}
	if (shown) printf("IOSwitch overlay: %u director%s from /private/var/ioswitch/overlay\n", shown, shown == 1 ? "y" : "ies");
}
