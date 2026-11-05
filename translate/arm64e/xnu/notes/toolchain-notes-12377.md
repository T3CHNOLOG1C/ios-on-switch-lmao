# Toolchain notes for building ioswitch on xnu-12377 (for the CMake/Bazel builds)

Measured with Apple clang 2100.3.20.102 (Xcode beta, iOS 27 SDK), round 5/6.

1. **Pin the core on the board line.** At an iOS 26+ deployment target, `-arch arm64` gets an apple-a12 feature set:
   LSE, LDAPR (RCPC), FP16, JSCVT, FCMA, RDM, and `__ARM_ARCH_8_2__`/`__ARM_ARCH_8_3__`/`__ARM_FEATURE_ATOMICS`
   defined. `-march=armv8-a` does not take any of it away (`-march=armv8-a+nolse+norcpc` neither); `-mcpu=cortex-a57`
   does. 12377 builds at 26/27 (10063 at 17.0, where none of this happens). So, in `makedefs/MakeInc.def`:

       MACHINE_FLAGS_ARM64_BCM2837 = -DARM64_BOARD_CONFIG_BCM2837 -mcpu=cortex-a57

   It must be in MACHINE_FLAGS, not CFLAGS_EXTRA: SFLAGS (assembly) has no CFLAGS_EXTRA, and `cswitch.s` /
   `machine_routines_asm.s` read UAO/DIT and use CASA under `__ARM_ARCH_8_2__`/`__ARM_ARCH_8_4__`. Keep
   `-march=armv8-a` in CFLAGS_EXTRA as today (with both, -march decides the features: no CRC, as in the 10063 build).
2. **Recipes derived from the kernel's compile records keep `-mcpu`.** `cmake/Drivers.cmake:21`
   `ioswitch_recipe(iokit_cxx_generic ... DROP -mcpu= -march= -flto)` must become `DROP -march= -flto`
   (port/ext_build.py does that): dropping `-mcpu` was harmless at 17.0 and gives NativeRamBlock.o LSE (4 words) at 26+.
3. **Check, don't assume.** After every kernel link, classify every instruction word (round 5's
   `port/verify/flagged_syms.py` with `build/a64class`, or `build/a64scan -A macho <kernel>`): the result must be the
   same 40 data/guarded words at the same symbols as the 10063 kernel (K256 and lz4 tables, data after the exception
   vectors, guarded impdef sysregs in awl_* and `_ioswitch_secondary_entry`). Before the pin it was 2,464 LSE + 403
   LDAPR words more. Our own objects (302 of them): 0.
4. **Command-line per-file flags replace the Makefile's.** `<file>.o_CFLAGS_ADD=` on the make command line overrides
   12377's per-file `$1_CFLAGS_ADD +=` (e.g. `-DVM_UNSAFE_TYPES` for bsd/vm/*.o): append theirs when setting ours.
5. **Deployment target for the final link**: `IPHONEOS_DEPLOYMENT_TARGET=27.0` in the environment (round 4); the
   Private.symbolset export check fails on BCM2837 (Apple DART/UAT symbols absent) without affecting the binary.
6. **Userland built for the Switch** (the `__TEXT,__a64x_target` marker means "never scanned"): with any iOS 26 SDK
   target use `-mcpu=cortex-a57 -mbranch-protection=none`, never `-march=armv8-a` alone.
