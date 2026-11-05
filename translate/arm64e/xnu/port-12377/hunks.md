# ios-on-switch-lmao XNU diff d8b8029..3515591 on xnu-12377.121.6: per-hunk result

336 hunks in 120 files: applied 212, kept ours 26, already in 12377 24, kept ours (hand-merged) 23, kept ours (relocated) 15, took 12377/UniXNU 12, kept ours (ported by hand) 10, dropped 9, kept ours (+12377) 3, applied (adapted) 2

| file | hunk | result | added lines present | reason |
|---|---:|---|---|---|
| `EXTERNAL_HEADERS/CoreEntitlements/CoreEntitlements.h` | 1 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `EXTERNAL_HEADERS/TrustCache/API.h` | 1 | already in 12377 | 166/166 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `EXTERNAL_HEADERS/TrustCache/RawTypes.h` | 1 | already in 12377 | 76/76 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `EXTERNAL_HEADERS/TrustCache/Return.h` | 1 | already in 12377 | 103/103 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `EXTERNAL_HEADERS/TrustCache/Types.h` | 1 | already in 12377 | 272/272 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `EXTERNAL_HEADERS/TrustCache/TypesConfig.h` | 1 | already in 12377 | 329/329 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `bsd/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/dev/arm/systemcalls.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/dev/arm/systemcalls.c` | 2 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/bsd_init.c` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/bsd_init.c` | 2 | applied | 76/76 added lines in the result | auto: 12377/UniXNU already has all of ios-on-switch-lmao's lines |
| `bsd/kern/bsd_init.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/bsd_init.c` | 4 | applied | 6/6 added lines in the result | auto: 12377/UniXNU already has all of ios-on-switch-lmao's lines |
| `bsd/kern/bsd_init.c` | 5 | kept ours | 27/27 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `bsd/kern/bsd_init.c` | 6 | kept ours | 22/22 added lines in the result | kept ours: with XNU_SWITCH_VERIFIED_ROOT the SD disk is published after bsd_autoconf, UniXNU's probe otherwise |
| `bsd/kern/bsd_init.c` | 7 | kept ours | 12/12 added lines in the result | kept ours: with XNU_SWITCH_VERIFIED_ROOT the SD disk is published after bsd_autoconf, UniXNU's probe otherwise |
| `bsd/kern/bsd_init.c` | 8 | kept ours | 45/45 added lines in the result | kept ours: raw-SD root authorization (owner-signed development policy for the exact OS root) on UniXNU's audit block |
| `bsd/kern/bsd_init.c` | 9 | kept ours | 25/25 added lines in the result | kept ours: raw-SD root authorization (owner-signed development policy for the exact OS root) on UniXNU's audit block |
| `bsd/kern/bsd_init.c` | 10 | applied | 9/9 added lines in the result | auto: 12377/UniXNU already has all of ios-on-switch-lmao's lines |
| `bsd/kern/kern_descrip.c` | 1 | applied | 10/10 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exec.c` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 4 | kept ours (relocated) | 3/3 added lines in the result | ios-on-switch-lmao's migration-process declaration placed before 12377's next function |
| `bsd/kern/kern_exec.c` | 5 | kept ours (relocated) | 13/13 added lines in the result | auto: ios-on-switch-lmao's inserted lines relocated into 12377's version of this code |
| `bsd/kern/kern_exec.c` | 6 | kept ours (relocated) | 4/4 added lines in the result | boot-progress/diag around load_machfile, relocated to 12377's call |
| `bsd/kern/kern_exec.c` | 7 | kept ours (ported by hand) | 146/146 added lines in the result | ios-on-switch-lmao's exec helpers (daemon environment, iconagent refusal, overlay/cryptex launch-constraint images) moved before exec_extract_strings: see POST |
| `bsd/kern/kern_exec.c` | 8 | kept ours (relocated) | 18/18 added lines in the result | iconagent refusal + daemon environment strings before the envv terminator, relocated |
| `bsd/kern/kern_exec.c` | 9 | kept ours | 982/982 added lines in the result | kept ours: SwitchRawMount.h and the owner/raw boot helpers (watchdog policy, migration, portrait, first-boot state) |
| `bsd/kern/kern_exec.c` | 10 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 11 | kept ours (relocated) | 6/6 added lines in the result | auto: ios-on-switch-lmao's inserted lines relocated into 12377's version of this code |
| `bsd/kern/kern_exec.c` | 12 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 13 | kept ours (hand-merged) | 49/49 added lines in the result | kept ours: UniXNU's 12377 owner mounts (init_envp, os_log) with ios-on-switch-lmao's boot progress, root events, watchdog policy and raw overlay |
| `bsd/kern/kern_exec.c` | 14 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 15 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 16 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 17 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 18 | applied | 5/5 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exec.c` | 19 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/kern_exit.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exit.c` | 2 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exit.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exit.c` | 4 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exit.c` | 5 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_exit.c` | 6 | applied | 14/14 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_memorystatus_freeze.c` | 1 | took 12377/UniXNU | 0/1 added lines in the result | 12377 already enables the freezer on iOS ((IOS && !XR) || WATCH); ios-on-switch-lmao only repaired 10063's broken #if |
| `bsd/kern/kern_priv.c` | 1 | applied | 9/9 added lines in the result | 3-way merge, no conflict |
| `bsd/kern/kern_proc.c` | 1 | kept ours (ported by hand) | 3/6 added lines in the result | 12377 replaced the TUNABLE by kern_feature_override; ours (tracking off with diagnostics) re-applied there: see POST; adapted: 12377 has no proc_ref_tracking_disabled TUNABLE; SWITCH_PROCREF_TRACKING_OFF makes every kern_feature_override(KF_DISABLE_PROCREF_TRACKING_OVRD) site off, as ours was |
| `bsd/kern/mach_loader.c` | 1 | took 12377/UniXNU | 3/4 added lines in the result | 12377 already maps 4K pages when !APPLE_ARM64_ARCH_FAMILY (same rule, new vm_map_pageshift variable) |
| `bsd/kern/source_pthread_bootstrap.c` | 1 | already in 12377 | 21/21 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `bsd/kern/sys_work_interval.c` | 1 | kept ours | 22/22 added lines in the result | 12377's sys/work_interval.h still lacks the INSTANCE_START/UPDATE/FINISH operations libsystem_kernel uses |
| `bsd/kern/sys_work_interval.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/sys_work_interval.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/sys_work_interval.c` | 4 | kept ours | 18/18 added lines in the result | 12377's sys/work_interval.h still lacks the INSTANCE_START/UPDATE/FINISH operations libsystem_kernel uses |
| `bsd/kern/ubc_subr.c` | 1 | applied | 1/1 added lines in the result | 12377 already includes vm/vm_map.h |
| `bsd/kern/ubc_subr.c` | 2 | kept ours (hand-merged) | 3/3 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 3 | kept ours (hand-merged) | 5/5 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 4 | kept ours (hand-merged) | 7/7 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 5 | kept ours (hand-merged) | 7/7 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 6 | kept ours (hand-merged) | 105/117 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 7 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/kern/ubc_subr.c` | 8 | kept ours (hand-merged) | 24/24 added lines in the result | took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters) plus ios-on-switch-lmao's skip when integrity checks are off |
| `bsd/kern/ubc_subr.c` | 9 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/miscfs/bindfs/bind_vfsops.c` | 1 | applied | 1/1 added lines in the result | UniXNU 12377 has native_bindfs_boot_mount and keeps BINDFS_ENTITLEMENT just below |
| `bsd/miscfs/bindfs/bind_vfsops.c` | 2 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/miscfs/bindfs/bind_vfsops.c` | 3 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/miscfs/bindfs/bind_vnops.c` | 1 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/bindfs/bindfs.h` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 3 | applied | 5/5 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 4 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 5 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 6 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 7 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.c` | 8 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.h` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.h` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_fsnode.h` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 2 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 3 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 4 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 5 | applied | 33/33 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 6 | applied | 6/6 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 7 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 8 | applied | 20/20 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vfsops.c` | 9 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 2 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 4 | applied | 6/6 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 5 | applied | 6/6 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 6 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `bsd/miscfs/mockfs/mockfs_vnops.c` | 7 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/net/necp_client.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/sys/make_symbol_aliasing.sh` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `bsd/sys/ubc_internal.h` | 1 | applied | 6/6 added lines in the result | 3-way merge, no conflict |
| `bsd/vfs/vfs_attrlist.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_attrlist.c` | 2 | kept ours (hand-merged) | 23/23 added lines in the result | kept ours: without content protection a valid class is a no-op; 12377's MACF check kept for the protected case |
| `bsd/vfs/vfs_attrlist.c` | 3 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_attrlist.c` | 4 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_attrlist.c` | 5 | kept ours (hand-merged) | 45/45 added lines in the result | kept ours: without content protection a valid class is a no-op; 12377's MACF check kept for the protected case |
| `bsd/vfs/vfs_attrlist.c` | 6 | kept ours (hand-merged) | 9/9 added lines in the result | kept ours: without content protection a valid class is a no-op; 12377's MACF check kept for the protected case |
| `bsd/vfs/vfs_cluster.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_cluster.c` | 2 | applied | 13/13 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_cluster.c` | 3 | applied | 4/4 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_cluster.c` | 4 | kept ours (hand-merged) | 1/1 added lines in the result | kept ours: cluster verification also for ios-on-switch-lmao's verified virtual-device mounts, on 12377's condition |
| `bsd/vfs/vfs_cluster.c` | 5 | applied | 132/132 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_cluster.c` | 6 | applied | 6/6 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_cluster.c` | 7 | applied | 6/6 added lines in the result | 3-way merge, no conflict in this hunk |
| `bsd/vfs/vfs_syscalls.c` | 1 | applied | 28/28 added lines in the result | 3-way merge, no conflict |
| `bsd/vfs/vfs_syscalls.c` | 2 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `bsd/vm/vm_unix.c` | 1 | kept ours (relocated) | 3/3 added lines in the result | page_decrypt.h for the home grid pager |
| `bsd/vm/vm_unix.c` | 2 | kept ours | 186/189 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept); adapted: home grid pager takes the text object's base offset (object_base), so it can back onto the a64x translator's object as well as the cache file's |
| `bsd/vm/vm_unix.c` | 3 | kept ours | 6/6 added lines in the result | kept ours: the OS cryptex mounted read-only at Cryptexes/OS counts as preboot for the shared cache |
| `bsd/vm/vm_unix.c` | 4 | kept ours | 21/21 added lines in the result | kept ours: the OS cryptex mounted read-only at Cryptexes/OS counts as preboot for the shared cache |
| `bsd/vm/vm_unix.c` | 5 | kept ours | 5/5 added lines in the result | kept ours: the OS cryptex mounted read-only at Cryptexes/OS counts as preboot for the shared cache |
| `config/MASTER` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `config/MASTER.arm64.bcm2837` | 1 | took 12377/UniXNU | 0/1 added lines in the result | UniXNU 12377 already adds nos_arm_asm nos_arm_pmap (via ARM_EXTRAS_BASE) |
| `config/MASTER.arm64.bcm2837` | 2 | kept ours (hand-merged) | 0/2 added lines in the result | kept ours: no sched_hygiene_debug in DEV/DEBUG (ios-on-switch-lmao removed it); 12377's other new options kept |
| `iokit/DriverKit/IOKernelReportStructs.h` | 1 | already in 12377 | 82/82 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/IOKitKeys.h` | 1 | dropped | 245/245 added lines in the result | 12377 ships its own newer copy of this DriverKit header |
| `iokit/DriverKit/IORPC.h` | 1 | already in 12377 | 218/218 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/IOReportTypes.h` | 1 | already in 12377 | 347/347 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/IOReturn.h` | 1 | already in 12377 | 177/177 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/IOTypes.h` | 1 | dropped | 214/214 added lines in the result | 12377 ships its own newer copy of this DriverKit header |
| `iokit/DriverKit/OSBoundedArray.h` | 1 | already in 12377 | 42/42 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/OSBoundedArrayRef.h` | 1 | already in 12377 | 41/41 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/OSBoundedPtr.h` | 1 | already in 12377 | 55/55 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/OSBoundedPtrFwd.h` | 1 | already in 12377 | 44/44 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/bounded_array.h` | 1 | already in 12377 | 108/108 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/bounded_array_ref.h` | 1 | already in 12377 | 274/274 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/bounded_ptr.h` | 1 | already in 12377 | 643/643 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/bounded_ptr_fwd.h` | 1 | already in 12377 | 36/36 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/crypto/aes.h` | 1 | already in 12377 | 98/98 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/crypto/md5.h` | 1 | already in 12377 | 73/73 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/crypto/sha1.h` | 1 | already in 12377 | 66/66 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/crypto/sha2.h` | 1 | already in 12377 | 64/64 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/macro_help.h` | 1 | already in 12377 | 82/82 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/DriverKit/queue_implementation.h` | 1 | dropped | 848/966 added lines in the result | 12377 ships its own newer copy of this DriverKit header |
| `iokit/DriverKit/safe_allocation.h` | 1 | already in 12377 | 445/445 added lines in the result | identical file already in the 12377 tree (UniXNU or 12377 itself) |
| `iokit/Kernel/IOLib.cpp` | 1 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `iokit/Kernel/IOMemoryDescriptor.cpp` | 1 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `iokit/Kernel/IOMemoryDescriptor.cpp` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `iokit/Kernel/IOService.cpp` | 1 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `iokit/Kernel/IOService.cpp` | 2 | applied | 10/10 added lines in the result | 3-way merge, no conflict |
| `iokit/Kernel/IOStartIOKit.cpp` | 1 | kept ours (ported by hand) | 6/6 added lines in the result | 12377 dropped the #ifndef BCM2837 around the IORTC wait; ours (switch_pmic_start) re-applied: see POST |
| `iokit/Kernel/IOStartIOKit.cpp` | 2 | kept ours (ported by hand) | 3/3 added lines in the result | 12377 dropped the #ifndef BCM2837 around the IORTC wait; ours (switch_pmic_start) re-applied: see POST |
| `iokit/Kernel/IOUserClient.cpp` | 1 | kept ours | 1/1 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `iokit/Kernel/IOUserClient.cpp` | 2 | took 12377/UniXNU | 0/5 added lines in the result | UniXNU 12377 already lets a native grant cover set-properties (with a NULL check) |
| `iokit/Kernel/IOUserClient.cpp` | 3 | kept ours | 34/34 added lines in the result | kept ours: grant list with '*' (all clients) and a NULL service check |
| `iokit/Kernel/IOUserClient.cpp` | 4 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `iokit/Kernel/IOUserClient.cpp` | 5 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `iokit/Kernel/IOUserClient.cpp` | 6 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `iokit/Kernel/IOUserClient.cpp` | 7 | kept ours | 74/74 added lines in the result | kept ours: grant list with '*' (all clients) and a NULL service check |
| `iokit/Kernel/IOUserClient.cpp` | 8 | kept ours | 18/18 added lines in the result | kept ours: grant list with '*' (all clients) and a NULL service check |
| `iokit/Kernel/IOUserClient.cpp` | 9 | kept ours | 60/60 added lines in the result | kept ours: grant list with '*' (all clients) and a NULL service check |
| `iokit/KernelConfigTables.cpp` | 1 | applied | 17/17 added lines in the result | 3-way merge, no conflict |
| `iokit/KernelConfigTables.cpp` | 2 | applied | 44/44 added lines in the result | 3-way merge, no conflict |
| `iokit/bsddev/DINetBootHook.cpp` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `iokit/bsddev/DINetBootHook.cpp` | 2 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `iokit/bsddev/IOKitBSDInit.cpp` | 1 | kept ours | 87/87 added lines in the result | kept ours: raw-SD cryptex manifest identity on top of UniXNU's boot manifest |
| `iokit/bsddev/IOKitBSDInit.cpp` | 2 | kept ours (relocated) | 4/4 added lines in the result | raw root wait diagnostic on 12377's iOS/XR branch |
| `iokit/bsddev/IOKitBSDInit.cpp` | 3 | kept ours (relocated) | 4/4 added lines in the result | raw root wait diagnostic on 12377's iOS/XR branch |
| `iokit/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `libkern/c++/OSKext.cpp` | 1 | applied | 4/4 added lines in the result | UniXNU 12377 has the same early security initializer, newer (PAC-safe call, result check) |
| `libkern/c++/OSKext.cpp` | 2 | took 12377/UniXNU | 18/36 added lines in the result | UniXNU 12377 has the same early security initializer, newer (PAC-safe call, result check) |
| `libkern/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `libsa/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `makedefs/MakeInc.def` | 1 | applied | 2/2 added lines in the result | UniXNU kernel-sdk-root.patch: same KERNEL_SDKROOT export, plus KDKROOT |
| `makedefs/MakeInc.def` | 2 | took 12377/UniXNU | 1/2 added lines in the result | UniXNU kernel-sdk-root.patch: same include paths (superset) |
| `makedefs/MakeInc.kernel` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm/arm_init.c` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 2 | kept ours (relocated) | 2/2 added lines in the result | auto: ios-on-switch-lmao's inserted lines relocated into 12377's version of this code |
| `osfmk/arm/arm_init.c` | 3 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 4 | applied | 4/4 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 5 | applied | 5/5 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 6 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 7 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 8 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 9 | kept ours (ported by hand) | 6/6 added lines in the result | boot progress before arm_vm_init: see POST (12377 moved the code around it) |
| `osfmk/arm/arm_init.c` | 10 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/arm_init.c` | 11 | applied | 8/8 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/commpage/commpage.c` | 1 | applied | 5/5 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm/cpuid.c` | 1 | applied | 9/9 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm/io_map.c` | 1 | took 12377/UniXNU | 0/4 added lines in the result | 12377 has no I/O submap any more (io_map allocates from kernel_map), so ours' 128 MiB enlargement has nothing to enlarge |
| `osfmk/arm/model_dep.c` | 1 | applied (adapted) | 112/113 added lines in the result | 3-way merge, no conflict; adapted: MAX_SCHED_CPUS -> MAX_CPUS (12377 removed MAX_SCHED_CPUS) |
| `osfmk/arm/pmap/pmap.c` | 1 | dropped | 4/4 added lines in the result | dropped: 12377 no longer calls uat_bootstrap() here, nothing to guard |
| `osfmk/arm/pmap/pmap.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 3 | dropped | 4/4 added lines in the result | dropped: 12377 no longer calls uat_bootstrap() here, nothing to guard |
| `osfmk/arm/pmap/pmap.c` | 4 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 5 | kept ours (hand-merged) | 12/12 added lines in the result | kept ours: dense-RAM mode skips reserved pages before handing out the next page |
| `osfmk/arm/pmap/pmap.c` | 6 | kept ours (hand-merged) | 24/24 added lines in the result | kept ours: dense-RAM mode skips reserved pages before handing out the next page |
| `osfmk/arm/pmap/pmap.c` | 7 | applied | 6/6 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 8 | kept ours (hand-merged) | 9/9 added lines in the result | kept ours: dense-RAM mode skips reserved pages before handing out the next page |
| `osfmk/arm/pmap/pmap.c` | 9 | applied | 3/3 added lines in the result | auto: 12377/UniXNU already has all of ios-on-switch-lmao's lines |
| `osfmk/arm/pmap/pmap.c` | 10 | took 12377/UniXNU | 0/4 added lines in the result | 12377 already prints the iommu ptep the same way |
| `osfmk/arm/pmap/pmap.c` | 11 | applied | 3/3 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 12 | kept ours (hand-merged) | 16/16 added lines in the result | kept ours: dense-RAM mode skips reserved pages before handing out the next page |
| `osfmk/arm/pmap/pmap.c` | 13 | applied | 3/3 added lines in the result | auto: 12377/UniXNU already has all of ios-on-switch-lmao's lines |
| `osfmk/arm/pmap/pmap.c` | 14 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 15 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.c` | 16 | kept ours (hand-merged) | 37/37 added lines in the result | kept ours: dense-RAM mode skips reserved pages before handing out the next page |
| `osfmk/arm/pmap/pmap.c` | 17 | kept ours (+12377) | 3/3 added lines in the result | SMP tables diagnostics include kept after 12377's new pmap_pending_preemption |
| `osfmk/arm/pmap/pmap.h` | 1 | applied | 5/5 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.h` | 2 | applied | 8/8 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm/pmap/pmap.h` | 3 | applied | 1/1 added lines in the result | padding field name only (12377 renumbered it reserved11) |
| `osfmk/arm/pmap/pmap_data.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm/pmap/pmap_data.h` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm/rtclock.c` | 1 | applied | 5/5 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/arm_vm_init.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 3 | applied | 96/96 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 4 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 5 | kept ours (hand-merged) | 1/1 added lines in the result | kept ours: write-combined physmap option on 12377's PTE type macro |
| `osfmk/arm64/arm_vm_init.c` | 6 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 7 | applied | 28/28 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 8 | applied | 6/6 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 9 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 10 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 11 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 12 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 13 | kept ours (relocated) | 6/6 added lines in the result | auto: ios-on-switch-lmao's inserted lines relocated into 12377's version of this code |
| `osfmk/arm64/arm_vm_init.c` | 14 | applied | 16/16 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/arm_vm_init.c` | 15 | applied | 4/4 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/caches_asm.s` | 1 | applied | 4/4 added lines in the result | UniXNU 12377 already defines CleanPoC_DcacheRegion_Force_nopreempt_nohid |
| `osfmk/arm64/cpu.c` | 1 | applied | 62/62 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/cpu.c` | 2 | applied | 8/8 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/cpu.c` | 3 | applied | 16/16 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/cpu.c` | 4 | applied | 6/6 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/genassym.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/switch_ram.h` | 1 | applied | 12/12 added lines in the result | new file |
| `osfmk/arm64/switch_smp.h` | 1 | applied | 70/70 added lines in the result | new file |
| `osfmk/arm64/switch_smp_tables.h` | 1 | applied | 50/50 added lines in the result | new file |
| `osfmk/arm64/machine_routines_asm.s` | 1 | applied | 67/67 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/machine_routines_asm.s` | 2 | applied | 11/11 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/proc_reg.h` | 1 | applied | 12/12 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/sleh.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/sleh.c` | 2 | kept ours (+12377) | 1/1 added lines in the result | boot progress for user breakpoints at the top of 12377's handler |
| `osfmk/arm64/sleh.c` | 3 | kept ours (hand-merged) | 1/1 added lines in the result | boot progress for user aborts before 12377's self-restrict check |
| `osfmk/arm64/sleh.c` | 4 | applied (adapted) | 122/124 added lines in the result | 3-way merge, no conflict in this hunk; adapted: task_best_name() returns const char * in 12377 (no caller buffer) |
| `osfmk/arm64/sleh.c` | 5 | applied | 5/5 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/sleh.c` | 6 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/sptm/pmap/pmap.c` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/arm64/start.s` | 1 | kept ours | 5/5 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 2 | kept ours (relocated) | 47/47 added lines in the result | Switch A57 PSCI first-start entry kept, after 12377's KTRR/CTRR/PV_CTRR #endif |
| `osfmk/arm64/start.s` | 3 | kept ours (relocated) | 26/26 added lines in the result | Switch A57 PSCI first-start entry kept, after 12377's KTRR/CTRR/PV_CTRR #endif |
| `osfmk/arm64/start.s` | 4 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/start.s` | 5 | kept ours | 80/80 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 6 | kept ours (relocated) | 346/346 added lines in the result | Switch A57 PSCI first-start entry kept, after 12377's KTRR/CTRR/PV_CTRR #endif |
| `osfmk/arm64/start.s` | 7 | kept ours (relocated) | 32/32 added lines in the result | Switch A57 PSCI first-start entry kept, after 12377's KTRR/CTRR/PV_CTRR #endif |
| `osfmk/arm64/start.s` | 8 | applied | 4/4 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/start.s` | 9 | kept ours | 72/72 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 10 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/start.s` | 11 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/start.s` | 12 | kept ours | 20/20 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 13 | kept ours | 38/38 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 14 | dropped | 0/1 added lines in the result | 12377 removed the Raspberry Pi core-timer routing block; ours disabled it under XNU_NATIVE_ARCH_TIMER anyway |
| `osfmk/arm64/start.s` | 15 | kept ours (relocated) | 5/5 added lines in the result | Switch A57 PSCI first-start entry kept, after 12377's KTRR/CTRR/PV_CTRR #endif |
| `osfmk/arm64/start.s` | 16 | kept ours | 48/48 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 17 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/arm64/start.s` | 18 | kept ours | 2/2 added lines in the result | auto: ios-on-switch-lmao extends the 12377/UniXNU block (all its lines kept) |
| `osfmk/arm64/start.s` | 19 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/conf/files.arm64` | 1 | took 12377/UniXNU | 3/4 added lines in the result | UniXNU 12377 already gates the AMCC files on apple_amcc |
| `osfmk/console/serial_console.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/serial_console.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 2 | applied | 24/24 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 3 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 4 | applied | 72/72 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 5 | applied | 19/19 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 6 | applied | 31/31 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 7 | applied | 27/27 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 8 | applied | 14/14 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 9 | applied | 17/17 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 10 | applied | 17/17 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 11 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 12 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 13 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.c` | 14 | applied | 77/77 added lines in the result | 3-way merge, no conflict |
| `osfmk/console/video_console.h` | 1 | applied | 5/5 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/Makefile` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/boot_progress.h` | 1 | applied | 34/34 added lines in the result | new file |
| `osfmk/kern/debug.c` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/debug.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/debug.c` | 3 | kept ours (hand-merged) | 2/2 added lines in the result | switch_panic_logged after 12377's SavePanicInfo (new initiator argument) |
| `osfmk/kern/debug.c` | 4 | applied | 4/4 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/kalloc.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/kalloc.c` | 2 | applied | 36/36 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/kalloc.c` | 3 | applied | 9/9 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/kalloc.c` | 4 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/kalloc.c` | 5 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/machine.c` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/machine.c` | 2 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/printf.c` | 1 | kept ours (hand-merged) | 1/2 added lines in the result | kept ours (no double framebuffer output) together with 12377's NULL check; adapted: same condition, plus 12377's PE_kputc != NULL check |
| `osfmk/kern/printf.c` | 2 | kept ours (hand-merged) | 1/2 added lines in the result | kept ours (no double framebuffer output) together with 12377's NULL check; adapted: same condition, plus 12377's PE_kputc != NULL check |
| `osfmk/kern/startup.c` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/startup.c` | 2 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/startup.c` | 3 | applied | 2/2 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/startup.c` | 4 | applied | 6/6 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/startup.c` | 5 | applied | 1/1 added lines in the result | 3-way merge, no conflict in this hunk |
| `osfmk/kern/startup.c` | 6 | took 12377/UniXNU | 0/1 added lines in the result | 12377 runs this trust-cache setup on every platform: what ours does with XNU_NATIVE_TRUST_CACHE=1 |
| `osfmk/kern/work_interval.c` | 1 | applied | 21/21 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/work_interval.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/work_interval.c` | 3 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/work_interval.h` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `osfmk/kern/zalloc.c` | 1 | dropped | 0/3 added lines in the result | dropped: PGZ defaults (ours: off with diagnostics) - 12377 has no PGZ |
| `osfmk/kern/zalloc.c` | 2 | dropped | 0/2 added lines in the result | dropped: every ios-on-switch-lmao hunk here is about PGZ (decode fix, alignment probe, default off); 12377 has no PGZ |
| `osfmk/kern/zalloc.c` | 3 | dropped | 14/81 added lines in the result | dropped: UniXNU's PGZ alignment probe (off by default) has no PGZ to probe in 12377 |
| `osfmk/prng/prng_random.c` | 1 | took 12377/UniXNU | 27/28 added lines in the result | UniXNU 12377's corecrypto14 bootstrap is the newer one (original corecrypto as sole provider) |
| `osfmk/vm/pmap.h` | 1 | applied | 9/9 added lines in the result | 3-way merge, no conflict |
| `osfmk/vm/pmap.h` | 2 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `osfmk/vm/vm_map.c` | 1 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `osfmk/vm/vm_resident.c` | 1 | kept ours (ported by hand) | 4/4 added lines in the result | dense-RAM page array ported by hand onto 12377's pmap_startup: see POST |
| `osfmk/vm/vm_resident.c` | 2 | kept ours (ported by hand) | 5/8 added lines in the result | dense-RAM page array ported by hand onto 12377's pmap_startup: see POST |
| `osfmk/vm/vm_resident.c` | 3 | kept ours (ported by hand) | 4/7 added lines in the result | dense-RAM page array ported by hand onto 12377's pmap_startup: see POST |
| `osfmk/vm/vm_resident.c` | 4 | kept ours (ported by hand) | 4/11 added lines in the result | dense-RAM page array ported by hand onto 12377's pmap_startup: see POST |
| `osfmk/vm/vm_resident.c` | 5 | kept ours (ported by hand) | 3/5 added lines in the result | dense-RAM page array ported by hand onto 12377's pmap_startup: see POST |
| `osfmk/vm/vm_shared_region.c` | 1 | kept ours (+12377) | 29/34 added lines in the result | kept ours: home grid patch next to 12377's a64x pager declaration (chained, see POST); adapted: vm_map_enter_mem_object has 12377's 13-argument form (sanitized address ref); patch takes the text object + base (a64x chaining) |
| `osfmk/vm/vm_shared_region.c` | 2 | took 12377/UniXNU | 3/4 added lines in the result | 12377 already sets the 4K page shift for the nested map on !APPLE_ARM64_ARCH_FAMILY |
| `osfmk/vm/vm_shared_region.c` | 3 | kept ours (hand-merged) | 4/6 added lines in the result | kept ours, chained: the home grid pager backs onto the a64x translator's object when there is one |
| `osfmk/vm/vm_user.c` | 1 | applied | 0/0 added lines in the result | 3-way merge, no conflict in this hunk |
| `pexpert/arm/pe_init.c` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_init.c` | 2 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_init.c` | 3 | applied | 17/17 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_init.c` | 4 | applied | 7/7 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_init.c` | 5 | applied | 3/3 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_kprintf.c` | 1 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `pexpert/arm/pe_kprintf.c` | 2 | applied | 2/2 added lines in the result | 3-way merge, no conflict |
| `pexpert/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `pexpert/pexpert/arm64/BCM2837.h` | 1 | applied | 4/4 added lines in the result | 3-way merge, no conflict |
| `pexpert/pexpert/arm64/board_config.h` | 1 | kept ours | 1/1 added lines in the result | kept ours: the Tegra X1 has one A57 cluster (UniXNU's SM8550 profile says 2) |
| `san/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
| `security/conf/Makefile.template` | 1 | applied | 1/1 added lines in the result | 3-way merge, no conflict |
