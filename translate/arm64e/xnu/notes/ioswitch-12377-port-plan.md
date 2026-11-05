# ioswitch's XNU diff (xnu-10063.141.1) on xnu-12377.121.6: what was done (round 5)

Source: `git -C out/native-a57/root-v1/tree/.cache/xnu-work diff d8b8029..ioswitch` at **3515591** (read only),
saved as `port/xnu-ioswitch-3515591.diff`: 120 files (31 added, 89 modified), 336 hunks, +11,520/-129.
Target: pristine xnu-12377.121.6 + UniXNU's 61-patch arm64 series + a64x (`scripts/prepare_xnu_build.py`).
Method: per file, `git merge-file` with base = 10063 (d8b8029), ours = the 12377 tree, theirs = ioswitch
(`port/merge.py`); conflicts resolved region by region (`port/resolve.py` + `port/decisions.py`, every decision
with its reason in `port/conflict-decisions.json`); per-hunk outcome in **`port/hunks.md`** (`port/hunk_table.py`).

Rule used (from the coordinator): keep our behaviour; where UniXNU's 12377 patch already does the same, take its
version and say so; where both diverge, keep ours and give one line on why (below).

## Result

- Files: 54 merged clean, 35 with conflicts (76 regions: 38 took 12377/UniXNU, 13 ours, 12 hand-merged, 10 ours
  relocated into 12377's code, 3 both), 4 new, 24 already identical in 12377/UniXNU, 3 DriverKit headers where 12377
  ships a newer copy (kept 12377's). 23 post-merge edits (12377 API changes, hand ports), 1 cut (below).
- Hunks (336): applied 212, applied with a 12377 API change 2, kept ours 26, kept ours hand-merged 23, kept ours
  relocated 15, kept ours ported by hand 10, kept ours + 12377 3, already in 12377 24, took 12377/UniXNU 12, dropped 9.
- The kernel links: `mach.development.bcm2837(.unstripped)`, Mach-O arm64 subtype ALL, NOUNDEFS, Darwin 25.5.0,
  with the same 15 link inputs as the main 10063 kernel (ioswitch's 11 driver/diag archives rebuilt against 12377).
- Diffs: `port/ioswitch-on-12377.diff` (ioswitch's delta over 12377+UniXNU+a64x: 58 files, +3,973/-791) and
  `port/xnu-12377-port-full.diff` (whole tree over pristine 12377: 119 files, +12,108/-130).

## Kept ours where UniXNU/12377 diverge (one line each)

- `bsd_init.c` #3: with XNU_IOSWITCH_VERIFIED_ROOT the SD disk is published after bsd_autoconf (our raw-SD root); UniXNU's probe stays for other builds.
- `bsd_init.c` #4: raw-SD root authorization (owner-signed development policy for the exact OS root) stays on top of UniXNU's audit block.
- `kern_exec.c` #7: IOSwitchRawMount.inc and the owner/raw boot helpers (watchdog policy, migration, portrait, first-boot state) are our mount path; UniXNU has none of it.
- `kern_exec.c` #8: UniXNU's owner mounts (init_envp, os_log) kept as the frame, with our boot progress, root events, watchdog policy and raw overlay inside.
- `kern_exec.c` cut (lines 7943-8971 of the 12377 side): our raw-SD `native_owner_cache_prepare`/`native_owner_data_prepare`/`native_gpu_addon_mount|prepare`/`native_owner_cache_checkpoint` replace UniXNU's same-named versions (two definitions cannot coexist); 17 UniXNU-only helpers only its versions called go with them: `native_cryptex_os_view/_bind`, `native_gestalt_cache_drop`, `native_ls_store_drop`, `native_staged_apps_*` (6), `native_gpu_addon_*` (7, Adreno add-on install). The cryptex/LS/gestalt ones are candidates to bring back once iOS 26 userland boots and shows it needs them.
- `sys_work_interval.c` #1: 12377's sys/work_interval.h still lacks INSTANCE_START/UPDATE/FINISH; ours handles operations 6-8 with an instance check, so UniXNU's catch-all for them is removed (POST).
- `ubc_subr.c` #2: took UniXNU's newer cs_native4k (1 MiB chunks, uncached above 32 MiB, timing counters; it also makes the cache read-only after filling, as ours did) plus our skip when integrity checks are off.
- `vfs_attrlist.c` #1: without content protection a valid class is a no-op (ours); 12377's MACF check kept for the protected case.
- `vfs_cluster.c` #1: cluster verification also for our verified virtual-device mounts, on 12377's condition.
- `vm_unix.c` #3: the OS cryptex mounted read-only at Cryptexes/OS counts as preboot for the shared cache (ours).
- `vm_unix.c` / `vm_shared_region.c` (3515591 home grid): chained onto the a64x translator: when the cache text has an a64x pager, the home grid apple_protect pager backs onto the a64x pager's object (offset `sms_file_offset`) instead of the file, so home-grid pages are patched after translation; otherwise exactly as in 3515591.
- `MASTER.arm64.bcm2837` #2: no sched_hygiene_debug in DEVELOPMENT/DEBUG (ours removed it); 12377's other new options kept.
- `IOUserClient.cpp` #3: grant list with '*' (all clients) and a NULL service check (ours; #2 took UniXNU's set-properties grant).
- `IOKitBSDInit.cpp` #1: raw-SD cryptex manifest identity on top of UniXNU's boot manifest.
- `IOStartIOKit.cpp` #1: 12377 dropped the `#ifndef BCM2837` around the IORTC wait; ours (switch_pmic_start sets the clock and publishes IORTC synchronously) re-applied (POST).
- `kern_proc.c` #1: proc-ref tracking off with diagnostics (a backtrace per proc_ref/rele); 12377 replaced the TUNABLE with kern_feature_override, so `IOSWITCH_PROCREF_TRACKING_OFF` short-circuits every site (POST).
- `pmap.c` #4: dense-RAM mode skips reserved pages before handing out the next page (on 12377's code).
- `vm_resident.c` #1-7: dense-RAM page array ported by hand onto 12377's linear `vm_pages` array/pmap_startup (holes become retired descriptors, usable pages reported) (POST).
- `arm_vm_init.c` #1: write-combined physmap option on 12377's PTE type macro.
- `printf.c` #1/#2: no double framebuffer output (ours), with 12377's `PE_kputc != NULL` check added.
- `debug.c` #1: `ioswitch_panic_logged` after 12377's SavePanicInfo (new initiator argument).
- `board_config.h` #1: the Tegra X1 has one A57 cluster (UniXNU's SM8550 profile says 2).
- `MakeInc.def` (new, not in the 10063 diff): `MACHINE_FLAGS_ARM64_BCM2837 += -mcpu=cortex-a57`. At 12377's iOS 26+ deployment target Apple clang gives arm64 an A12 feature set (LSE, LDAPR, FP16, `__ARM_ARCH_8_2__/8_3__`) that `-march=armv8-a` does not remove; 10063 builds with 17.0 and never needed it.

## Took UniXNU's / 12377's version (same behaviour already there)

`mach_loader.c` #1-2 and `vm_shared_region.c` #2 (4K page shift on !APPLE_ARM64_ARCH_FAMILY, via 12377's
`vm_map_create_with_page_shift`); `kern_memorystatus_freeze.c` (freezer already on for iOS); `OSKext.cpp` #1 (same
early security initializer, PAC-safe); `prng_random.c` (UniXNU's newer corecrypto14 bootstrap); `IOUserClient.cpp`
#2; `MakeInc.def` #1-2 (kernel-sdk-root.patch); `MASTER` #1 (nos_arm_asm/nos_arm_pmap); `caches_asm.s`,
`files.arm64` (AMCC gating), `pmap.c` #5-6 (iommu ptep print), `pmap.h` (padding name), `startup.c` (trust cache
setup now unconditional), `ubc_subr.c` #1, `bind_vfsops.c` #1; `vm_user.c` (only silenced an unused parameter).

## Dropped (nothing left to apply in 12377)

`zalloc.c` (all three hunks: PGZ decode fix, alignment probe, PGZ default off; 12377 has no PGZ); `io_map.c`
(12377 has no I/O submap, so the 128 MiB enlargement has nothing to enlarge); `start.s` #3 (12377 removed the RPi
core-timer routing block ours disabled); `pmap.c` #3 (12377 no longer calls uat_bootstrap there); 3 DriverKit
headers (12377's copies are newer).

## Adapted to 12377 APIs (mechanical)

`model_dep.c` MAX_SCHED_CPUS -> MAX_CPUS; `sleh.c` task_best_name returns const char *; `vm_shared_region.c`
13-argument vm_map_enter_mem_object with sanitized address; `pmap_data.h` declaration before use; proclog terminates
p_comm before printing (12377 renamed the length variable); in the ioswitch tree copies: `IOSwitchFiles.c` includes
vm_kern_xnu.h/mach_vm.h, `IOSwitchRootPolicy.inc` uses mach_vm_protect (vm_map_protect is VM-internal in 12377),
`mbuf_kpi_shim.h` for SwitchWLANNet (m_add_hdr_crumb C++ declarations); vm_unix.o keeps 12377's -DVM_UNSAFE_TYPES.

## Not verified

Nothing booted (no iOS 26 install path yet). Semantic drift is possible where hunks merged cleanly but the
surrounding 12377 code changed meaning (VM and exec most of all). `original-bindings.S` still binds iOS 17 kernelcache
addresses; libpthread_kernel.a, the corecrypto-2023 archive and libfirehose_kernel are the main tree's 10063-era
prebuilts.
