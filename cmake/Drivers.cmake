# Everything of ours the kernel links: which sources make which object, with which flags,
# and the member order of every archive. Objects and archives go where the tree's XNU
# build script and its scripts/build-*.py builders expect them, in ${IOSWITCH_TREE}/build.
#
# Not built here (inputs read from ${IOSWITCH_TREE}/build):
#   * original-bindings.S files, trusted-key.S and the generated ABI headers
#     (OriginalLegacyFramebufferABI.h, OriginalHidDeviceABI.h, OriginalPMGRABI.h,
#     GuardedPMGRSlots.inc, storage-abi/include, native-ram-block/NativeDataSeed.h). They come
#     from audits of the original kernelcache by scripts/build-native-legacy-display.py,
#     build-native-hid-input.py, build-platform-power.py, build-ioswitch-root-block.py,
#     build-native-ram-block.py, build-ioswitch-root-crypto.py and build-storage-abi.py;
#     rerun the matching script when a class gains an override or an original import.
#   * Apple's code built by other trees' scripts: corecrypto-2023 (build-corecrypto-14.py),
#     libpthread-kernel (build-libpthread-kernel.py) and libfirehose_kernel (build-firehose.sh).
include_guard(GLOBAL)

set(T "${IOSWITCH_TREE}")
set(B "${IOSWITCH_TREE}/build")

ioswitch_recipe(iokit_cxx RECORD iokit/DEVELOPMENT/IOPlatformExpert.cpo.json)
ioswitch_recipe(iokit_cxx_generic RECORD iokit/DEVELOPMENT/IOPlatformExpert.cpo.json DROP -mcpu= -march= -flto)
ioswitch_recipe(osfmk_c RECORD osfmk/DEVELOPMENT/arm_init.o.json)
ioswitch_recipe(bsd_c RECORD bsd/DEVELOPMENT/bsd_init.o.json KEEP_WARNINGS)
ioswitch_recipe(mockfs_c RECORD bsd/DEVELOPMENT/vfs_conf.o.json KEEP_WARNINGS DROP -DMOCKFS= -UMOCKFS)

# IOKit C++ (scripts/build-arm-driver.py): the kernel's C++ flags, our warnings, extra flags.
set(IOSWITCH_DRIVER_WARNINGS -Wall -Wextra -Werror -Wno-unused-parameter -Wno-c99-designator
    -Wno-reorder-init-list -Wno-deprecated-enum-enum-conversion -Wno-unnecessary-virtual-specifier)
function(ioswitch_driver object source)
  ioswitch_compile("${object}" RECIPE iokit_cxx SOURCE "${source}"
    FLAGS ${IOSWITCH_DRIVER_WARNINGS} -I "${IOSWITCH_TREE}/.cache/xnu-work/osfmk" ${ARGN})
endfunction()

# Diagnostics C (the IOSwitchDiag archive recipe): the kernel's osfmk C flags, no FP/SIMD.
set(IOSWITCH_DIAG_FLAGS -DIOSWITCH_XNU_DIAGNOSTICS=1 -DXNU_IOSWITCH_VERIFIED_ROOT=1 -DIOSWITCH_SD_MODEL=0
    -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin
    -Wall -Wextra -Werror -Wno-unused-parameter
    -I "${T}/diagnostics/ioswitch" -I "${T}/kits/VerifiedRoot/include")

# --- ArmCpu, ArmDTPlatform: CPU/GIC/timer and platform expert -------------------------------
ioswitch_driver("${B}/armcpu/ArmCpu.o" "${T}/drivers/ArmCpu/ArmCpu.cpp")
ioswitch_driver("${B}/armcpu/ArmGIC.o" "${T}/drivers/ArmGIC/ArmGIC.cpp")
ioswitch_archive("${B}/armcpu/libArmCpu.a" "${B}/armcpu/ArmCpu.o" "${B}/armcpu/ArmGIC.o")

ioswitch_driver("${B}/armdt-platform/ArmDTPlatform.o" "${T}/drivers/ArmDTPlatform/ArmDTPlatform.cpp")
ioswitch_archive("${B}/armdt-platform/libArmDTPlatform.a" "${B}/armdt-platform/ArmDTPlatform.o")

# --- NativeAES ---------------------------------------------------------------------------
ioswitch_driver("${B}/native-aes/NativeAES.o" "${T}/drivers/NativeAES/NativeAES.cpp")
ioswitch_bindings("${B}/native-aes/original-bindings.o" "${T}/scripts/data/native-aes-original-bindings.S")
ioswitch_archive("${B}/native-aes/libNativeAES.a"
  "${B}/native-aes/NativeAES.o" "${B}/native-aes/original-bindings.o")

# --- TegraGM20B: the Tegra X1 GPU (vendored nouveau nvkm under an XNU shim, IOAcceleratorES + AGXDeviceUserClient) -------
#     Inert unless tegra-gpu=<stage>; ioswitch_root_boot_prepare() calls tegra_gm20b_probe(). Needs the IOGPUFamily prelink
#     and drivers/IOSwitchAccelStub (bindings) in the same kernel. The C units take the osfmk C
#     recipe without its include options: the shim is freestanding, and XNU's headers clash with its Linux-named ones
#     (ptrcheck's __counted_by, ...); drivers/TegraGM20B/build-tegra-gm20b.py does the same.
set(gpu "${B}/tegra-gm20b")
set(gpu_src "${T}/drivers/TegraGM20B")
file(STRINGS "${IOSWITCH_RECIPES}/osfmk_c.rsp" osfmk_args)
set(gpu_rsp "")
set(skip FALSE)
foreach(arg IN LISTS osfmk_args)
  if(skip)
    set(skip FALSE)
  elseif(arg MATCHES "^\"-(I|include|isystem)\"$")
    set(skip TRUE)
  else()
    string(APPEND gpu_rsp "${arg}\n")
  endif()
endforeach()
file(WRITE "${IOSWITCH_RECIPES}/tegra_gm20b_c.rsp.new" "${gpu_rsp}")
configure_file("${IOSWITCH_RECIPES}/tegra_gm20b_c.rsp.new" "${IOSWITCH_RECIPES}/tegra_gm20b_c.rsp" COPYONLY)
set(IOSWITCH_RECIPE_tegra_gm20b_c_CC "${IOSWITCH_RECIPE_osfmk_c_CC}")
set(IOSWITCH_RECIPE_tegra_gm20b_c_DIR "${IOSWITCH_RECIPE_osfmk_c_DIR}")
# Apple clang's kernel defaults add -Wsign-conversion, -Wshorten-64-to-32 and -Wimplicit-int-conversion (as warnings);
# Linux-style nvkm code and its shim trip them over a thousand times.
set(gpu_c_flags -mgeneral-regs-only -mstrict-align -fno-stack-protector -std=gnu11
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-conversion -Wno-shorten-64-to-32 -Wno-implicit-int-conversion
    -I "${gpu_src}/shim/include" -I "${gpu_src}/nouveau/include" -I "${gpu_src}/nouveau/include/nvkm"
    -I "${gpu_src}/nouveau/nvkm" -I "${gpu_src}/nouveau/nvkm/subdev/gsp" -I "${gpu_src}/nouveau"
    -include "${gpu_src}/shim/include/nvxnu_prelude.h")
set(gpu_objects "")
file(GLOB_RECURSE gpu_nouveau CONFIGURE_DEPENDS "${gpu_src}/nouveau/*.c")
foreach(source IN LISTS gpu_nouveau)
  file(RELATIVE_PATH name "${gpu_src}" "${source}")
  string(REPLACE "/" "_" name "${name}")
  # the relaxations Linux builds nvkm with (and two Apple clang defaults upstream nvkm trips)
  ioswitch_compile("${gpu}/${name}.o" RECIPE tegra_gm20b_c SOURCE "${source}"
    FLAGS ${gpu_c_flags} -Wno-pointer-sign -Wno-sign-compare -Wno-missing-field-initializers -Wno-shadow
          -Wno-conditional-uninitialized)
  list(APPEND gpu_objects "${gpu}/${name}.o")
endforeach()
foreach(source IN ITEMS shim/linux.c shim/tegra_shim.c shim/stubs.c shim/nvif_nvkm.c platform.c gpu.c)
  string(REPLACE "/" "_" name "${source}")
  ioswitch_compile("${gpu}/${name}.o" RECIPE tegra_gm20b_c SOURCE "${gpu_src}/${source}" FLAGS ${gpu_c_flags})
  list(APPEND gpu_objects "${gpu}/${name}.o")
endforeach()
foreach(source IN ITEMS shim/xnu_glue.cpp TegraGM20B.cpp TegraGM20BUserClient.cpp)
  get_filename_component(stem "${source}" NAME_WE)
  ioswitch_driver("${gpu}/${stem}.o" "${gpu_src}/${source}" -I "${gpu_src}")
  list(APPEND gpu_objects "${gpu}/${stem}.o")
endforeach()
# AGXAccelerator subclasses the original IOAcceleratorES: a late object, run by TegraGM20B.cpp after IOGPUFamily. Its three
# IOAcceleratorES imports are bound by the accelerator stub's original-bindings.o (libNativeLegacyDisplay.late.a, same link).
ioswitch_driver("${gpu}/TegraGM20BAccelerator.o" "${gpu_src}/TegraGM20BAccelerator.cpp" -I "${gpu_src}")
ioswitch_late_object("${gpu}/TegraGM20BAccelerator.late.o" "${gpu}/TegraGM20BAccelerator.o" TegraGM20BAccelerator.cpp)
list(APPEND gpu_objects "${gpu}/TegraGM20BAccelerator.late.o")
# GM20B falcon firmware (linux-firmware nvidia/gm20b, 15 files) by .incbin
ioswitch_compile_command("${gpu}/firmware.o" SOURCE "${gpu_src}/firmware.S"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -Werror -I "${gpu_src}/firmware")
list(APPEND gpu_objects "${gpu}/firmware.o")

# --- BlueTool stand-in for the userland overlay (tree/overlay/manifest.json, gated on switch-bt) -------------
set(bt_tool "${B}/switch-bluetooth/BlueTool")
set(bt_src "${T}/drivers/SwitchBluetooth/BlueTool")
add_custom_command(OUTPUT "${bt_tool}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${B}/switch-bluetooth"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -Os -Wall -Wextra -Werror
          -o "${bt_tool}" "${bt_src}/BlueTool.c"
  COMMAND codesign -f -s - --identifier com.apple.BlueTool --entitlements "${bt_src}/BlueTool.entitlements" "${bt_tool}"
  DEPENDS "${bt_src}/BlueTool.c" "${bt_src}/BlueTool.entitlements"
  COMMENT "Building the BlueTool stand-in" VERBATIM)
add_custom_target(switch-bluetooth-overlay ALL DEPENDS "${bt_tool}")

# --- SwitchAudio HAL plug-in for the userland overlay (AudioServerPlugIn; the iOS SDK lacks its headers, so the
#     macOS CoreAudio ones, which declare the same interface, are linked in under H/CoreAudio) --------------------
set(audio_out "${B}/switch-audio")
set(audio_hal "${T}/drivers/SwitchAudio/hal")
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path OUTPUT_VARIABLE IOSWITCH_MACOSX_SDK
                OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
add_custom_command(OUTPUT "${audio_out}/SwitchAudio.driver/SwitchAudio"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${audio_out}/H/CoreAudio" "${audio_out}/SwitchAudio.driver"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink "${IOSWITCH_MACOSX_SDK}/System/Library/Frameworks/CoreAudio.framework/Headers/AudioServerPlugIn.h" "${audio_out}/H/CoreAudio/AudioServerPlugIn.h"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink "${IOSWITCH_MACOSX_SDK}/System/Library/Frameworks/CoreAudio.framework/Headers/AudioHardwareBase.h" "${audio_out}/H/CoreAudio/AudioHardwareBase.h"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink "${IOSWITCH_MACOSX_SDK}/System/Library/Frameworks/CoreAudio.framework/Headers/AudioHardware.h" "${audio_out}/H/CoreAudio/AudioHardware.h"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -I "${audio_out}/H" -I "${T}/drivers/SwitchAudio"
          -O2 -Wall -Wextra -Werror -bundle -fvisibility=hidden -framework CoreFoundation -framework IOKit
          "${audio_hal}/SwitchAudioPlugIn.c" -o "${audio_out}/SwitchAudio.driver/SwitchAudio"
  COMMAND codesign -f -s - --pagesize 4096 --identifier com.ioswitch.audio "${audio_out}/SwitchAudio.driver/SwitchAudio"
  DEPENDS "${audio_hal}/SwitchAudioPlugIn.c" "${T}/drivers/SwitchAudio/SwitchAudioAbi.h"
  COMMENT "Building the SwitchAudio HAL plug-in" VERBATIM)
add_custom_target(switch-audio-overlay ALL DEPENDS "${audio_out}/SwitchAudio.driver/SwitchAudio")

# --- HEVCH8.videodecoder: Apple's software HEVC decoder (VCPHEVC) in VideoToolbox's hardware slot (userland overlay,
#     gated on switch-hevc). drivers/SwitchVideoDecoders/HEVCH8.c says why.
set(video "${B}/switch-video")
add_custom_command(OUTPUT "${video}/HEVCH8.videodecoder"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${video}"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -O2 -Wall -Wextra -Werror
          -dynamiclib -fvisibility=hidden -install_name /System/Library/VideoDecoders/HEVCH8.videodecoder
          -o "${video}/HEVCH8.videodecoder" "${T}/drivers/SwitchVideoDecoders/HEVCH8.c"
  COMMAND codesign -f -s - --pagesize 4096 --identifier com.ioswitch.hevch8 "${video}/HEVCH8.videodecoder"
  DEPENDS "${T}/drivers/SwitchVideoDecoders/HEVCH8.c"
  COMMENT "Building HEVCH8.videodecoder" VERBATIM)
# AVD.videodecoder: H.264 on NVDEC as VideoToolbox's hardware 'avc1' decoder (overlay, in the VideoDecoders union
# gated switch-hevc; it registers only when the kernel published TegraNVDEC, switch-nvdec=3). vdectest + its launchd
# plist: the layer-2 check, run once when TegraNVDEC appears (results in the unified log, subsystem com.ioswitch.avd,
# and /private/var/ioswitch/vdectest.log).
set(avd "${T}/drivers/SwitchVideoDecoders/AVD")
set(avd_sources "${avd}/AVDDecoder.c" "${avd}/H264NVDEC.c" "${avd}/H264Syntax.c" "${avd}/NVDECClient.c" "${avd}/BlockLinear.c")
add_custom_command(OUTPUT "${video}/AVD.videodecoder"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${video}"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -O2 -Wall -Wextra -Werror
          -dynamiclib -fvisibility=hidden -install_name /System/Library/VideoDecoders/AVD.videodecoder
          -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOKit
          -o "${video}/AVD.videodecoder" ${avd_sources}
  COMMAND codesign -f -s - --pagesize 4096 --identifier com.ioswitch.avd "${video}/AVD.videodecoder"
  DEPENDS ${avd_sources} "${avd}/H264NVDEC.h" "${avd}/H264Syntax.h" "${avd}/NVDECClient.h" "${avd}/BlockLinear.h"
          "${avd}/VTDecoderSPI.h" "${T}/drivers/TegraNVDEC/TegraNVDECABI.h"
  COMMENT "Building AVD.videodecoder" VERBATIM)
set(vdectest_src "${T}/drivers/SwitchVideoDecoders/test")
add_custom_command(OUTPUT "${video}/vdectest"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${video}"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -O2 -Wall -Wextra -Werror
          -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -lz
          -o "${video}/vdectest" "${vdectest_src}/vdectest.c"
  COMMAND codesign -f -s - --identifier com.ioswitch.vdectest "${video}/vdectest"
  DEPENDS "${vdectest_src}/vdectest.c" "${vdectest_src}/VDECClip.h"
  COMMENT "Building vdectest" VERBATIM)
add_custom_target(switch-video-overlay ALL DEPENDS "${video}/HEVCH8.videodecoder" "${video}/AVD.videodecoder"
                  "${video}/vdectest")

# --- NativeIOSwitchBlock: the SD card's block device (+ cryptex files) --------------------
set(block "${B}/native-ioswitch-root")
set(block_flags -mgeneral-regs-only -mstrict-align -I "${B}/storage-abi/include"
    -I "${T}/diagnostics/ioswitch/storage" -I "${T}/diagnostics/ioswitch" -I "${T}/kits/VerifiedRoot/include")
ioswitch_driver("${block}/NativeIOSwitchBlockDevice.o" "${T}/drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp" ${block_flags})
ioswitch_driver("${block}/IOSwitchCryptex.o" "${T}/drivers/NativeIOSwitchBlock/IOSwitchCryptex.cpp" ${block_flags})
ioswitch_driver("${block}/SwitchNVRAM.o" "${T}/drivers/SwitchNVRAM/SwitchNVRAM.cpp" ${block_flags})
ioswitch_bindings("${block}/original-bindings.o" "${block}/original-bindings.S")
# Board drivers (GPIO, I2C, fan/thermal, battery). They ride in the block archive, which the XNU link
# already takes, and are pulled in by the board thread in IOSwitchRootDiag.inc.
set(board "${B}/switch-board")
ioswitch_driver("${board}/TegraGPIO.o" "${T}/drivers/TegraGPIO/TegraGPIO.cpp")
ioswitch_driver("${board}/TegraI2C.o" "${T}/drivers/TegraI2C/TegraI2C.cpp")
ioswitch_driver("${board}/SwitchThermal.o" "${T}/drivers/SwitchPower/SwitchThermal.cpp")
ioswitch_driver("${board}/SwitchBattery.o" "${T}/drivers/SwitchBattery/SwitchBattery.cpp")
ioswitch_driver("${board}/SwitchBluetooth.o" "${T}/drivers/SwitchBluetooth/SwitchBluetooth.cpp")
# SwitchAudio: kernel service + Tegra I2S1/ADMA/RT5639 backend (switch-audio=1|3; null sink otherwise).
ioswitch_driver("${board}/SwitchAudio.o" "${T}/drivers/SwitchAudio/SwitchAudio.cpp")
# Boot clock line, MAX77620 RTC (IORTC) and power-off/restart; called from IOKitInitializeTime() (xnu patch).
ioswitch_driver("${board}/SwitchPMIC.o" "${T}/drivers/SwitchPower/SwitchPMIC.cpp")
# System sleep (SC7) coordinator, inert unless switch-sleep=2; started last on the board thread.
ioswitch_driver("${board}/SwitchSuspend.o" "${T}/drivers/SwitchSuspend/SwitchSuspend.cpp")
# SwitchWLAN: BCM4356A3 Wi-Fi on PCIe port 1 (opt-in, boot-arg switch-wlan=1). firmware.S .incbins the blobs in
# drivers/SwitchWLAN/firmware: touch it when they change.
set(wlan "${B}/switch-wlan")
set(wlan_objects "")
foreach(unit IN ITEMS SwitchWLAN SwitchWLANProbe TegraPCIe SwitchWLANChip SwitchWLANMsgbuf SwitchWLANCrypto SwitchWLANWpa SwitchWLANNet)
  ioswitch_driver("${wlan}/${unit}.o" "${T}/drivers/SwitchWLAN/${unit}.cpp" -mgeneral-regs-only -mstrict-align)
  list(APPEND wlan_objects "${wlan}/${unit}.o")
endforeach()
ioswitch_compile_command("${wlan}/firmware.o" SOURCE "${T}/drivers/SwitchWLAN/firmware.S"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -Werror
          -I "${T}/drivers/SwitchWLAN/firmware")
list(APPEND wlan_objects "${wlan}/firmware.o")
# --- TegraBPMP: the T210 BPMP mailbox (L4T mail.c / mail_t210.c / mailman.c vendored under a shim) and the EMC clock
#     through MRQ_CLK. Place after the TegraGM20B block (reuses its freestanding tegra_gm20b_c recipe) and add
#     ${bpmp_objects} to the libNativeIOSwitchBlockDevice.a member list. Called by SwitchPMIC (switch_pmic_start).
set(bpmp_src "${T}/drivers/TegraBPMP")
set(bpmp_c_flags -mgeneral-regs-only -mstrict-align -fno-stack-protector -std=gnu11
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-conversion -Wno-shorten-64-to-32 -Wno-implicit-int-conversion
    -Wno-sign-compare -Wno-missing-field-initializers -Wno-gnu-empty-struct -Wno-zero-length-array
    -I "${bpmp_src}/shim/include" -I "${bpmp_src}/linux/include" -I "${bpmp_src}/linux/drivers/firmware/tegra"
    -I "${T}/drivers" -include "${bpmp_src}/shim/include/bpxnu_prelude.h")
set(bpmp_objects "")
foreach(source IN ITEMS linux/drivers/firmware/tegra/mail.c linux/drivers/firmware/tegra/mail_t210.c
                        linux/drivers/firmware/tegra/mailman.c shim/linux.c TegraBPMP.c)
  string(REPLACE "/" "_" name "${source}")
  ioswitch_compile("${board}/tegra-bpmp_${name}.o" RECIPE tegra_gm20b_c SOURCE "${bpmp_src}/${source}" FLAGS ${bpmp_c_flags})
  list(APPEND bpmp_objects "${board}/tegra-bpmp_${name}.o")
endforeach()
ioswitch_driver("${board}/TegraBPMPGlue.o" "${bpmp_src}/shim/xnu_glue.cpp")
list(APPEND bpmp_objects "${board}/TegraBPMPGlue.o")

# --- TegraDFLL: T210B01 CPU DFLL (closed-loop CL-DVFS), L4T clk-dfll.c + fcpu/cvb/speedo vendored under a Linux shim.
#     Inert until something calls tegra_dfll_cpu_* (SwitchThermal, switch-cpu-mode=dfll). Same C recipe as TegraGM20B
#     (osfmk C flags minus -I/-include/-isystem), its own include set and prelude; the vendored units get the same
#     relaxations as nvkm.
set(dfll "${B}/tegra-dfll")
set(dfll_src "${T}/drivers/TegraDFLL")
set(dfll_c_flags -mgeneral-regs-only -mstrict-align -fno-stack-protector -std=gnu11
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-conversion -Wno-shorten-64-to-32 -Wno-implicit-int-conversion
    -I "${dfll_src}/shim/include" -I "${dfll_src}/linux/include" -include "${dfll_src}/shim/include/tdfll_prelude.h")
set(dfll_objects "")
foreach(source IN ITEMS linux/drivers/clk/tegra/clk-dfll.c linux/drivers/clk/tegra/clk-tegra124-dfll-fcpu.c
                        linux/drivers/soc/tegra/cvb.c linux/drivers/soc/tegra/fuse/speedo-tegra210.c)
  string(REPLACE "/" "_" name "${source}")
  ioswitch_compile("${dfll}/${name}.o" RECIPE tegra_gm20b_c SOURCE "${dfll_src}/${source}"
    FLAGS ${dfll_c_flags} -Wno-pointer-sign -Wno-sign-compare -Wno-missing-field-initializers -Wno-shadow
          -Wno-conditional-uninitialized)
  list(APPEND dfll_objects "${dfll}/${name}.o")
endforeach()
foreach(source IN ITEMS shim/linux.c platform.c dfll.c)
  string(REPLACE "/" "_" name "${source}")
  ioswitch_compile("${dfll}/${name}.o" RECIPE tegra_gm20b_c SOURCE "${dfll_src}/${source}" FLAGS ${dfll_c_flags})
  list(APPEND dfll_objects "${dfll}/${name}.o")
endforeach()
ioswitch_driver("${dfll}/shim_xnu_glue.cpp.o" "${dfll_src}/shim/xnu_glue.cpp" -I "${dfll_src}/shim")
list(APPEND dfll_objects "${dfll}/shim_xnu_glue.cpp.o")

# --- TegraHost1x + TegraNVDEC: host1x syncpoints/channel 0, the NVDEC falcon, its job engine, buffer arena, boot
#     self-test and user client (inert unless switch-host1x / switch-nvdec=1..3; TegraNVDEC.h has the stages);
#     NVDEC's firmware (linux-firmware nvidia/tegra210/nvdec.bin) by .incbin. Exports tegra_nvdec_suspend/resume.
set(nvdec "${B}/tegra-nvdec")
ioswitch_driver("${nvdec}/TegraHost1x.o" "${T}/drivers/TegraHost1x/TegraHost1x.cpp")
set(nvdec_objects "${nvdec}/TegraHost1x.o")
foreach(unit IN ITEMS TegraNVDEC TegraNVDECMemory TegraNVDECSelfTest TegraNVDECUserClient)
  ioswitch_driver("${nvdec}/${unit}.o" "${T}/drivers/TegraNVDEC/${unit}.cpp")
  list(APPEND nvdec_objects "${nvdec}/${unit}.o")
endforeach()
ioswitch_compile_command("${nvdec}/nvdec_firmware.o" SOURCE "${T}/drivers/TegraNVDEC/firmware.S"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -Werror
          -I "${T}/drivers/TegraNVDEC/firmware")
list(APPEND nvdec_objects "${nvdec}/nvdec_firmware.o")

ioswitch_archive("${block}/libNativeIOSwitchBlockDevice.a"
  "${block}/NativeIOSwitchBlockDevice.o" "${block}/original-bindings.o" "${block}/IOSwitchCryptex.o"
  "${block}/SwitchNVRAM.o" "${board}/TegraGPIO.o" "${board}/TegraI2C.o" "${board}/SwitchThermal.o"
  "${board}/SwitchBattery.o" "${board}/SwitchBluetooth.o" "${board}/SwitchPMIC.o" "${board}/SwitchSuspend.o" "${board}/SwitchAudio.o" ${wlan_objects} ${gpu_objects} ${bpmp_objects} ${dfll_objects} ${nvdec_objects})

# --- NativeRamBlock (scripts/build-native-ram-block.py). The XNU script links the object
#     itself next to the block driver's bindings and only checks the archive. ---------------
set(ram "${B}/native-ram-block")
ioswitch_compile("${ram}/NativeRamBlock.o" RECIPE iokit_cxx_generic SOURCE "${T}/drivers/NativeRamBlock/NativeRamBlock.cpp"
  FLAGS ${IOSWITCH_DRIVER_WARNINGS} -march=armv8-a -mgeneral-regs-only -fno-lto
        -I "${T}/.cache/xnu-work/osfmk" -I "${B}/storage-abi/include" -I "${ram}")
ioswitch_bindings("${ram}/original-bindings.o" "${ram}/original-bindings.S")
ioswitch_archive("${ram}/libNativeRamBlock.a" "${ram}/NativeRamBlock.o" "${ram}/original-bindings.o")
set_property(GLOBAL APPEND PROPERTY IOSWITCH_LINK_INPUTS "${ram}/NativeRamBlock.o")

# --- Verified root policy (scripts/build-ioswitch-root-crypto.py) ------------------------
set(verified "${B}/verified-root-ioswitch")
foreach(source IN ITEMS src/verified_root.c adapters/xnu_crypto.c)
  get_filename_component(stem "${source}" NAME_WE)
  ioswitch_compile("${verified}/${stem}.o" RECIPE bsd_c SOURCE "${T}/kits/VerifiedRoot/${source}"
    FLAGS "-I${T}/kits/VerifiedRoot/include" -Werror -mgeneral-regs-only -mstrict-align)
endforeach()
ioswitch_bindings("${verified}/trusted-key.o" "${verified}/trusted-key.S")
ioswitch_archive("${verified}/libVerifiedRootProbe.a"
  "${verified}/verified_root.o" "${verified}/xnu_crypto.o" "${verified}/trusted-key.o")

# --- XNU's mockfs (scripts/build-mockfs-fixture.py) ------------------------------------
set(mockfs_objects "")
foreach(name IN ITEMS mockfs_fsnode mockfs_vfsops mockfs_vnops)
  ioswitch_compile("${B}/mockfs-fixture/${name}.o" RECIPE mockfs_c
    SOURCE "${T}/.cache/xnu-work/bsd/miscfs/mockfs/${name}.c" FLAGS -DMOCKFS=1)
  list(APPEND mockfs_objects "${B}/mockfs-fixture/${name}.o")
endforeach()
ioswitch_archive("${B}/mockfs-fixture/libmockfs_fixture.a" ${mockfs_objects})

# --- ShengPlatformPower: PMGR subclass (scripts/build-platform-power.py) ----------------
set(pmgr "${B}/sheng-platform-power")
ioswitch_driver("${pmgr}/ShengPlatformPower.o" "${T}/kits/qemu/ShengPlatformPower.cpp" -I "${pmgr}")
ioswitch_late_object("${pmgr}/ShengPlatformPower.late.o" "${pmgr}/ShengPlatformPower.o" ShengPlatformPower.cpp)
ioswitch_bindings("${pmgr}/original-bindings.o" "${pmgr}/original-bindings.S")
ioswitch_archive("${pmgr}/libShengPlatformPower.a" "${pmgr}/ShengPlatformPower.late.o" "${pmgr}/original-bindings.o")

# --- IOSwitchDiag: boot diagnostics, SD/FAT32/APFS storage --------------------------------
set(diag_objects "")
foreach(source IN ITEMS IOSwitchDiag.c IOSwitchPanicSave.c IOSwitchFiles.c IOSwitchSD.c storage/block_backend.c storage/fat32.c
                        storage/partition.c storage/sd_reference.c storage/sdmmc_read.c storage/split_disk.c
                        storage/data_backend.c storage/raw_apfs.c storage/sdmmc_dma.c)
  get_filename_component(stem "${source}" NAME_WE)
  ioswitch_compile("${B}/ioswitch-diag/${stem}.o" RECIPE osfmk_c
    SOURCE "${T}/diagnostics/ioswitch/${source}" FLAGS ${IOSWITCH_DIAG_FLAGS})
  list(APPEND diag_objects "${B}/ioswitch-diag/${stem}.o")
endforeach()
ioswitch_archive("${B}/ioswitch-diag/libIOSwitchDiag.a" ${diag_objects})

# --- Link stubs: no-op probe hooks (scripts/build-link-stubs.py) --------------------------
ioswitch_compile_command("${B}/link-stubs/LinkStubs.o" SOURCE "${T}/kits/qemu/LinkStubs.c"
  COMMAND xcrun --sdk iphoneos clang -arch arm64 -mkernel -miphoneos-version-min=17.0 -Os -Wall -Werror)
ioswitch_archive("${B}/link-stubs/libLinkStubs.a" "${B}/link-stubs/LinkStubs.o")

# --- Display, key store and HID input: one late-initialized archive -----------------------
#     (scripts/build-native-legacy-display.py, reduced to what the native board links)
set(display "${B}/native-legacy-display")
set(keystore "${B}/native-keystore-events")
set(hid "${B}/native-hid-input")
set(gop "${B}/native-gop-presenter")
ioswitch_driver("${display}/NativeLegacyDisplay.o" "${T}/drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp" -I "${display}")
ioswitch_late_object("${display}/NativeLegacyDisplay.late.o" "${display}/NativeLegacyDisplay.o" NativeLegacyDisplay.cpp)
# The display across SC7 (tegra_display_suspend/resume): hekate's cold display init/power-down, called by NativeLegacyDisplay.
ioswitch_driver("${display}/TegraDisplayPower.o" "${T}/drivers/NativeMobileDisplay/TegraDisplayPower.cpp")
ioswitch_bindings("${display}/original-bindings.o" "${display}/original-bindings.S")
foreach(probe IN ITEMS LegacyDisplayProbe KeyStoreProbe HidInputProbe TouchState)
  ioswitch_driver("${display}/${probe}.o" "${T}/kits/qemu/${probe}.cpp")
endforeach()
ioswitch_driver("${keystore}/NativeKeyStoreEvents.o" "${T}/drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp")
ioswitch_late_object("${keystore}/NativeKeyStoreEvents.late.o" "${keystore}/NativeKeyStoreEvents.o" NativeKeyStoreEvents.cpp)
ioswitch_driver("${hid}/NativeHidInput.o" "${T}/drivers/NativeHidInput/NativeHidInput.cpp" -I "${hid}")
ioswitch_late_object("${hid}/NativeHidInput.late.o" "${hid}/NativeHidInput.o" NativeHidInput.cpp)
ioswitch_bindings("${hid}/hid-original-bindings.o" "${hid}/original-bindings.S")
ioswitch_driver("${gop}/NativeGOPPresenter.o" "${T}/drivers/NativeMobileDisplay/NativeGOPPresenter.cpp")
ioswitch_late_object("${gop}/NativeGOPPresenter.late.o" "${gop}/NativeGOPPresenter.o" NativeGOPPresenter.cpp)
# --- Touch screen and buttons. SwitchHIDDevice subclasses the original IOHIDDevice through
#     NativeHidInput's bindings (hid-original-bindings.o, same archive), so it is a late object.
set(input "${B}/switch-input")
ioswitch_driver("${input}/SwitchHIDDevice.o" "${T}/drivers/SwitchTouch/SwitchHIDDevice.cpp" -I "${hid}")
ioswitch_late_object("${input}/SwitchHIDDevice.late.o" "${input}/SwitchHIDDevice.o" SwitchHIDDevice.cpp)
ioswitch_driver("${input}/SwitchTouch.o" "${T}/drivers/SwitchTouch/SwitchTouch.cpp")
ioswitch_driver("${input}/SwitchButtons.o" "${T}/drivers/SwitchButtons/SwitchButtons.cpp")
# --- Backlight (CoreBrightness's AppleARMBacklight, by metaclass), display power and the BH1730 ambient light sensor.
#     SwitchALS calls SwitchHIDDevice (this archive); the bindings are absolute symbols (AppleARMPlatform's metaclass).
set(brightness "${B}/switch-brightness")
foreach(unit IN ITEMS TegraPanel SwitchBacklight SwitchALS)
  ioswitch_driver("${brightness}/${unit}.o" "${T}/drivers/SwitchBrightness/${unit}.cpp")
endforeach()
ioswitch_bindings("${brightness}/original-bindings.o" "${T}/drivers/SwitchBrightness/original-bindings.S")
# --- Accelerator stub: ends QuartzCore's 30 s IOAcceleratorES poll (drivers/IOSwitchAccelStub) -----------
set(accel "${B}/accel-stub")
ioswitch_driver("${accel}/IOSwitchAccelStub.o" "${T}/drivers/IOSwitchAccelStub/IOSwitchAccelStub.cpp")
ioswitch_late_object("${accel}/IOSwitchAccelStub.late.o" "${accel}/IOSwitchAccelStub.o" IOSwitchAccelStub.cpp)
ioswitch_bindings("${accel}/original-bindings.o" "${T}/drivers/IOSwitchAccelStub/original-bindings.S")
ioswitch_driver("${accel}/IOSwitchAccelStubPublish.o" "${T}/drivers/IOSwitchAccelStub/IOSwitchAccelStubPublish.cpp")
# AppleKeyStore stand-in's device key: RFC 3394 wrap under a key derived from the fuse chip ID.
ioswitch_driver("${keystore}/AKSDeviceKey.o" "${T}/drivers/NativeKeyStoreEvents/AKSDeviceKey.cpp")
# --- Joy-Cons (switch-joycon-rail / switch-joycon-bt). SwitchJoyConHID subclasses the original IOHIDDevice through
#     NativeHidInput's bindings (same archive), so it is a late object.
set(joycon "${B}/switch-joycon")
ioswitch_driver("${joycon}/SwitchJoyConHID.o" "${T}/drivers/SwitchJoyCon/SwitchJoyConHID.cpp" -I "${hid}")
ioswitch_late_object("${joycon}/SwitchJoyConHID.late.o" "${joycon}/SwitchJoyConHID.o" SwitchJoyConHID.cpp)
ioswitch_driver("${joycon}/SwitchJoyConRail.o" "${T}/drivers/SwitchJoyCon/SwitchJoyConRail.cpp")
# joyconpaird: hands rail-made Bluetooth pairings to bluetoothd (userland overlay, gated on switch-joycon-bt).
set(joycon_helper "${T}/drivers/SwitchJoyCon/helper")
add_custom_command(OUTPUT "${joycon}/joyconpaird"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${joycon}"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -O2 -Wall -Wextra -Werror
          -Wno-unused-parameter -framework CoreFoundation -framework IOKit -o "${joycon}/joyconpaird" "${joycon_helper}/joyconpaird.c"
  COMMAND codesign -s - -f --entitlements "${joycon_helper}/joyconpaird.entitlements" "${joycon}/joyconpaird"
  DEPENDS "${joycon_helper}/joyconpaird.c" "${joycon_helper}/joyconpaird.entitlements"
  COMMENT "Building joyconpaird" VERBATIM)
# joyconnfc: reads NFC tags through a Joy-Con (R) / Pro Controller's MCU (overlay, launched only with switch-joycon-nfc).
add_custom_command(OUTPUT "${joycon}/joyconnfc"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${joycon}"
  COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -O2 -Wall -Wextra -Werror
          -Wno-unused-parameter -framework CoreFoundation -framework IOKit -o "${joycon}/joyconnfc" "${joycon_helper}/joyconnfc.c"
  COMMAND codesign -s - -f --entitlements "${joycon_helper}/joyconnfc.entitlements" "${joycon}/joyconnfc"
  DEPENDS "${joycon_helper}/joyconnfc.c" "${joycon_helper}/joyconnfc.entitlements"
  COMMENT "Building joyconnfc" VERBATIM)
add_custom_target(switch-joycon-overlay ALL DEPENDS "${joycon}/joyconpaird" "${joycon}/joyconnfc")
ioswitch_archive("${display}/libNativeLegacyDisplay.late.a"
  "${display}/NativeLegacyDisplay.late.o" "${display}/TegraDisplayPower.o" "${display}/original-bindings.o"
  "${display}/LegacyDisplayProbe.o" "${display}/KeyStoreProbe.o" "${display}/HidInputProbe.o" "${display}/TouchState.o"
  "${keystore}/NativeKeyStoreEvents.late.o" "${hid}/NativeHidInput.late.o" "${hid}/hid-original-bindings.o"
  "${gop}/NativeGOPPresenter.late.o"
  "${input}/SwitchHIDDevice.late.o" "${input}/SwitchTouch.o" "${input}/SwitchButtons.o"
  "${brightness}/TegraPanel.o" "${brightness}/SwitchBacklight.o" "${brightness}/SwitchALS.o" "${brightness}/original-bindings.o"
  "${joycon}/SwitchJoyConHID.late.o" "${joycon}/SwitchJoyConRail.o"
  "${accel}/IOSwitchAccelStubPublish.o" "${accel}/IOSwitchAccelStub.late.o" "${accel}/original-bindings.o"
  "${keystore}/AKSDeviceKey.o")

# Prebuilt inputs must exist before the XNU link.
foreach(input IN ITEMS
    "${B}/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a"
    "${B}/libpthread-kernel/libpthread_kernel.a" "${B}/libpthread-kernel/source_pthread_bootstrap.o"
    "${T}/.cache/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a"
    "${B}/storage-abi/include" "${display}/OriginalLegacyFramebufferABI.h" "${hid}/OriginalHidDeviceABI.h"
    "${pmgr}/OriginalPMGRABI.h" "${pmgr}/GuardedPMGRSlots.inc")
  if(NOT EXISTS "${input}")
    message(FATAL_ERROR "Missing prebuilt kernel input ${input} (see the comment at the top of ${CMAKE_CURRENT_LIST_FILE})")
  endif()
endforeach()
