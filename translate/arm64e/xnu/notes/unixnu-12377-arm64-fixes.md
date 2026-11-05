# Building UniXNU's xnu-12377 series as plain arm64 (BCM2837, Cortex-A57) with a64x

Order: pristine xnu-12377.121.6, then UniXNU `patches/xnu-12377/*` in the order of
`profiles/arch/arm64e/patches/xnu/series`, skipping:
- `native-pac-keys.patch`, `arm64e-threaded-starts.patch` (PAC hardware / arm64e kernel only),
- `native-arm-pan.patch` (does not apply without the PAC patch; the A57 has no PAN anyway),
then `scripts/patch_xnu_a64x.py` (or `a64x-xnu-12377.diff`) and the a64x core files, then:

1. `EXTERNAL_HEADERS/TrustCache/{API,RawTypes,Return,Types,TypesConfig}.h` from xnu-8796.101.5 (UniXNU's profile does
   the same: `trustcache_tag = "xnu-8796.101.5"`).
2. `osfmk/kern/debug.c` (`unixnu_fileset_unmapped`, added by `fileset-unmapped-call.patch`): replace
   `__builtin_ptrauth_strip(__builtin_return_address(0), 0)` with `__builtin_return_address(0)`; the builtin does not
   exist on a non-ptrauth target.
3. `config/MASTER.arm64.bcm2837`: add `a64x` to VM_BASE (next to `config_code_decryption`).

Build:

    IPHONEOS_DEPLOYMENT_TARGET=27.0 make SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 \
      KERNEL_CONFIGS=DEVELOPMENT ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
      XNU_AVAILABILITY_PL=<fakeroot>/usr/local/libexec/availability.pl KERNEL_SDKROOT=<fakeroot> \
      OBJROOT=... SYMROOT=... DSTROOT=... RC_DARWIN_KERNEL_VERSION=25.5.0 RC_ProjectSourceVersion=12377.121.6 \
      BUILD_WERROR=0 MAKEJOBS=-j10

(`IPHONEOS_DEPLOYMENT_TARGET` in the environment: without it the final kernel link defaults to macOS and refuses
the iOS objects. `<fakeroot>` = the main tree's `.cache/fakeroot`, used read only.)

Result (2026-11-04): `mach.development.bcm2837.unstripped`, Mach-O arm64 (CPU subtype ALL, no PAC), 21.5 MB,
NOUNDEFS, a64x inside (kern.a64x sysctls, strings). The only failure is the post-link `check_all_exports`
(`Private.symbolset`: Apple DART/UAT symbols `t8020dart_*`, `uat_get_desc` that no BCM2837 kernel has), which only
matters for linking Apple's kexts against the symbol sets; the UniXNU/ios-on-switch-lmao prelinker does its own binding.
