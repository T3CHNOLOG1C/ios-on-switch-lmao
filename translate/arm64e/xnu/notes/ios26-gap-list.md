# Booting iOS 26 userland (iPadOS 26.5.2 23F84, iPad11,6 / J171aAP): gap list (round 6)

Target as chosen in round 2 (NOTES 1): `iPad_10.2_2020_26.5.2_23F84_Restore.ipsw` (iPad 8, A12, Lightning, home
button; kernelcache `kernelcache.release.ipad11b`, xnu-12377.122.8 RELEASE_ARM64_T8020, kexts built for 23F82).
Today's userland: 17.7.11 21H461 from `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (iPad7,1, A10X, j120ap). Each
item below says what it is, the evidence, and what to do. "Ours" is the ios-on-switch-lmao 12377 port (round 5).

## A. The 17 UniXNU helpers cut from kern_exec (round 5): what comes back

| helper(s) | what UniXNU uses it for | needed for 26.5.2? | form |
|---|---|---|---|
| `native_cryptex_os_view`, `native_cryptex_os_bind` | binds the SystemOS cryptex's /System and /usr at /private/preboot/Cryptexes/OS so the root's /System/Cryptexes/OS links resolve (WebKit's ExtensionKit processes) | No as code. Ours mounts the two cryptex volumes at /private/preboot/Cryptexes/{OS,App} itself (SwitchRawMount.h), which is what those links reach; 23F84 uses the same cryptex scheme (NOTES 5). | Keep ours. Check once on 26 that `/System/Cryptexes/OS/System/Library/ExtensionKit/Extensions/WebContentExtension.appex` resolves. |
| `native_gestalt_cache_drop` | deletes MobileGestalt's cache once after a device-tree answer changed (P3 -> sRGB) | Not on a fresh 26 Data volume (no stale cache). Needed if the ADT identity or display answers change on an installed Data volume. | Not code: the installer (or a one-shot marker like ours) removes `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` when the boot package's ADT identity changes. |
| `native_ls_store_drop` | rebuilds LaunchServices' database once after it was built while /System/Cryptexes/OS led nowhere | No: on 26 the cryptex path exists from the first boot (ours mounts before launchd). | None. |
| `native_staged_apps_*` (6) | copies OS-volume /private/var/staged_system_apps into Data before launchd | No as code: our installer seeds Data from the System volume's /private/var (incl. staged_system_apps) and ours triggers DataMigrator with its sentinel. | Keep ours; the migration prefs must name 23F84 (B2). |
| `native_gpu_addon_*` (7) | Adreno add-on installation (SM8550) | Never (Tegra). | None. |

## B. What overlay / launchd policy / kern_exec assume about 17.7.11 (21H461)

Identifiers that are per IPSW and are compiled in today (all must become generated from the IPSW pins, the way
`tools/install/spec.py` already reads them back):
1. **System volume UUID** `04769D26-2CA0-4825-B443-E2E5F88300F2` (21H461's OS container): `NativeSwitchBlock.cpp`
   `kRootUUID` (also the ADT root-matching UUID), `SwitchRawMount.h:172` and `kern_exec.c` (owner boot manifest).
   23F84's OS volume has its own UUID.
2. **SystemOS cryptex volume UUID** `8846FE78-...` (`SwitchCryptex.h`, `SwitchRawMount.h:173`) and App cryptex
   `C0DCFAA8-...`; **cryptex image names** `044-76034-038.dmg` / `044-75188-038.dmg` (`SwitchCryptex.cpp` paths) ->
   23F84: `094-54813-109.dmg` (SystemOS, after AEA decryption) and `094-54871-112.dmg` (AppOS).
3. **First-boot prefs** (`kern_exec.c` around 8930): `com.apple.migration` LastSystemVersion/buildVersion `21H461`
   -> `23F84`; `com.apple.purplebuddy` `SetupVersion 11` -> 23F84's value (read it from Setup.app / UniXNU's
   skip-setup seed for its 23F84 profile); the 29 staged apps are the 21H461 set (23F84's list comes from the image).
4. **Cryptex-only code directories** (`SwitchRawMount.h` `cryptex_only[]`, "21H461: these are all of them"):
   recompute for 23F84 by diffing the SystemOS cryptex against the OS volume (directories with Mach-Os only in the
   cryptex).
5. **launchd override database** (`kern_exec.c` ~9050): "the only override database 21H461 launchd reads" and the
   disabled labels (nfcd, seld, coreidvd, biometrickitd.mesa, nearbyd, tailspind, ...). Re-read which files 26's
   launchd consults and re-check every label (26 adds and renames daemons; anything for absent hardware must be
   listed again).
6. **AMFI launch constraints** (`native_switch_overlay_image`, `native_switch_cryptex_image`; UniXNU's
   zzyc-owner-skip-launch-constraints): written against 21H461 AMFI's self constraint (on-system-volume or authorized
   authapfs volume, launch-type 1, validation-category 1). With 26's AMFI kext the constraint set may differ; check the
   `Launch Constraint Violation` lines of the first 26 boot against these two exceptions.
7. **QuartzCore / CoreImage / bluetoothd internals** keyed to 21H461 code: `CA_FORCE_TIMER` (CADeviceUseVBL chip ids
   0x7000/0x8010), `CA_DISABLE_DETACHED` (ca_debug_options[21]), `BT_HCI_TRANSPORT` (bluetoothd's POSIX HCI driver),
   the iconservicesagent refusal (Core Image's GLES fallback). Each must be re-verified in 23F84's binaries (offsets in
   the comments are 21H461's).
8. **Restore-created Data directories** (keybags, containers; UniXNU prepare-offline-data): re-check against 23F84's
   usermanagerd / containermanagerd.
9. **Userland is arm64e** (A12+): 17.7.11 on iPad7,1 is plain arm64, so nothing in today's boot path exercises the
   a64x translator. On 26 every process does: exec/mmap translation, the dyld cache plan
   (`dyld_shared_cache_arm64e` + 83 subcaches; plan `out/dsc26.a64xplan`, SHA-256 da87ada7...c650c841), MAP_JIT refused
   for arm64e (JavaScriptCore without JIT). The prelinked kexts are arm64 after translation (this round).
10. **Device identity**: MobileGestalt and the ipad11b device class expect J171aAP / iPad11,6 in the ADT (product-type,
    target-type, model, `chip-id 0x8020`), not j120ap.
11. **Kernel side from this round** (prelink26): 379 Apple kernel functions and 68 data symbols the 48 kexts use have
    no name yet (go to a panic trampoline / NULL): 11,491 of 142,195 kernel-reference sites. AMFI has no early
    security initializer address for 23F84 (17's `--early-security-init AMFI=0xfffffff005ab4ae0` has no 26 value yet).
    `AppleUSBXHCI` is not in J171a's kernelcache (Lightning iPad), so SwitchXHCI has no class to subclass;
    `IOMobileFramebufferLegacy` grew 358 -> 369 vtable slots and `IOUSBDeviceController` 234 -> 242, so
    NativeLegacyDisplay's and SwitchXUDC's class declarations (their opaque slots) must be regenerated from the 26
    vtables before their bindings can be complete (bindings: 211 of 510 resolved, prelink26/bindings/).

## C. What a 26.5.2 variant of the install image must produce (for the Bazel pipeline)

Same steps as `tools/install/sd.py` (compose / write / reprobe / populate / boot / verify); the differences:
1. **Pins** (`bazel/ipsw/pins.bzl`, second set): url
   `https://updates.cdn-apple.com/2026SpringFCS/fullrestores/140-26213/ECD4BB1B-84CD-4C37-A46D-F5B9907DF621/iPad_10.2_2020_26.5.2_23F84_Restore.ipsw`,
   build `23F84`, product `iPad11,6`, device class `j171aap`; members: OS `094-56745-108.dmg.aea` (7.18 GB),
   SystemOS cryptex `094-54813-109.dmg.aea` (1.98 GB, 5.19 GB decrypted), AppOS `094-54871-112.dmg` (14 MB), their
   three `Firmware/*.trustcache`, `kernelcache.release.ipad11b`, `Firmware/all_flash/DeviceTree.j171aap.im4p`.
2. **AEA decryption** (new): OS and SystemOS are Apple Encrypted Archives. Key from the AEA header's key URL
   (`ipsw fw aea --key`, needs network once) -> pin the key's SHA-256 next to the member; decrypt with macOS `aea
   decrypt` streaming (nothing but the decrypted image on disk, as round 2 did). The pinned `content_sha256` is of the
   decrypted image.
3. **Kernelcache**: keep it whole (`--thin-arm64` does not apply: the 26 cache is an arm64e MH_FILESET); it feeds
   `prelink26/emit26.py` (not NativePrelink) and the bindings regeneration.
4. **Sizes**: the 32 GiB `IOSW_RAW_BYTES` partition has to hold OS (decrypted, ~8 GB grown), Preboot (5.19 GB SystemOS
   + 14 MB AppOS + plan) and Data; keep 32 GiB as a minimum and measure the composed image before fixing it.
5. **Preboot extra**: `/a64x/dyld_shared_cache_arm64e.a64xplan` (20.9 MB, made by `tools/a64plan2` from the decrypted
   SystemOS cryptex's cache; pinned SHA-256), and `/chosen/a64x-plan-sha256` in the boot package's ADT.
6. **Generated kernel constants** (B1-B2): the installer must not rewrite the System volume UUID; the kernel and ADT
   take theirs from the pins (spec.json the other way round: generated header, not parsed source).
7. **Data seeding**: from 23F84's System /private/var, as today; plus the B3 prefs with 23F84 values.
8. **Boot package**: kernel = our 12377 kernel prelinked with the 26 kexts (`prelink26/out/kernel.macho` shape),
   26 original-bindings linked into it, ADT with the J171aAP identity and the 26 volume UUIDs; same single Hekate
   entry `switchroot/ios` (user rule) with the 17.7 / 26 choice made by the native loader (NOTES 5).
9. **Trust**: static trust cache bundle = 23F84's OS + SystemOS trust caches; App cryptex trust cache appended, as
   for 17.7.11.
