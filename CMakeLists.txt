# IOSwitch: the native XNU kernel for the Switch and its Hekate boot entry.
#
#   cmake -S . -B out/cmake -G Ninja
#   cmake --build out/cmake                     # kernel + package (default)
#   cmake --build out/cmake --target kernel     # drivers, XNU, prelink -> out/cmake/kernel/kernel.macho
#   cmake --build out/cmake --target package    # -> out/boot/ioswitch (IOSWITCH_BOOT_DIR)
#   cmake --build out/cmake --target deploy     # -> the SD card at IOSWITCH_SD, then eject
#
# Boot-args and ADT edits live in scripts/package_raw_boot.py; XNU options in the pass's
# build-options.json (IOSWITCH_XNU_OPTIONS); what our drivers are made of in cmake/Drivers.cmake.
cmake_minimum_required(VERSION 3.25)
project(IOSwitch LANGUAGES NONE)

set(IOSWITCH_OUT "${CMAKE_SOURCE_DIR}/out")
set(IOSWITCH_OPTIONS_TREE "${IOSWITCH_OUT}/native-a57/root-v1/tree")
set(IOSWITCH_TREE "${IOSWITCH_OPTIONS_TREE}" CACHE PATH
    "Kernel tree: XNU source (.cache/xnu-work), drivers, kits, diagnostics and build/")
set(IOSWITCH_PASS "${IOSWITCH_OUT}/native-a57/root-v1/passes/native-root-smp-v1" CACHE PATH
    "Pass whose build-options.json, prelink-command.json and prelink tool the kernel uses")
set(IOSWITCH_XNU_OPTIONS "${IOSWITCH_PASS}/build-options.json" CACHE FILEPATH
    "XNU_* options for the XNU build script (paths in it below the main tree are moved to IOSWITCH_TREE)")
set(IOSWITCH_XNU_SCRIPT "scripts/build-xnu-persistent-data-v17-fast-active.sh" CACHE STRING
    "XNU build script, relative to IOSWITCH_TREE")
set(IOSWITCH_PRELINK_TOOL "${IOSWITCH_PASS}/source/prelink/emit_prelink.py" CACHE FILEPATH
    "Prelinker for Apple's original kexts")
option(IOSWITCH_SMP "Package for four CPUs (package_raw_boot.py --smp)" OFF)
set(IOSWITCH_BOOT_DIR "${IOSWITCH_OUT}/boot/ioswitch" CACHE PATH
    "Where the package target writes the boot entry (switchroot/ and bootloader/)")
set(IOSWITCH_SD "/Volumes/SWITCH SD" CACHE PATH "Mounted SD card the deploy target writes")
set(IOSWITCH_TOOL_PATH "/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin" CACHE STRING
    "Directories put first on PATH for the XNU build")
find_program(IOSWITCH_LLVM_OBJCOPY llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
find_program(IOSWITCH_PYTHON python3 REQUIRED)

foreach(path IN ITEMS "${IOSWITCH_TREE}/.cache/xnu-work" "${IOSWITCH_TREE}/${IOSWITCH_XNU_SCRIPT}"
                      "${IOSWITCH_XNU_OPTIONS}" "${IOSWITCH_PRELINK_TOOL}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${path} does not exist")
  endif()
endforeach()

include(cmake/XnuRecipes.cmake)
include(cmake/Drivers.cmake)
include(cmake/Kernel.cmake)
include(cmake/Package.cmake)

# Lint and format (scripts/lint.py, .clang-format/.clang-tidy in the tree); never part of the default build.
#   cmake --build out/cmake --target compdb        # <tree>/compile_commands.json for clang-tidy and clangd
#   cmake --build out/cmake --target format-check  # files clang-format would change
#   cmake --build out/cmake --target format        # format every first-party file in place
#   cmake --build out/cmake --target tidy          # clang-tidy over every first-party unit
set(IOSWITCH_LINT "${CMAKE_SOURCE_DIR}/scripts/lint.py")
add_custom_target(compdb COMMAND "${IOSWITCH_PYTHON}" "${IOSWITCH_LINT}" compdb USES_TERMINAL)
add_custom_target(format-check COMMAND "${IOSWITCH_PYTHON}" "${IOSWITCH_LINT}" format --check --all USES_TERMINAL)
add_custom_target(format COMMAND "${IOSWITCH_PYTHON}" "${IOSWITCH_LINT}" format --all USES_TERMINAL)
add_custom_target(tidy COMMAND "${IOSWITCH_PYTHON}" "${IOSWITCH_LINT}" tidy --all --compdb USES_TERMINAL)
