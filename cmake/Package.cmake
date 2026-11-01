# The Hekate boot entry (scripts/package_raw_boot.py: boot-args, ADT edits, kernel window,
# loader, boot.scr) and its deployment to the SD card.
include_guard(GLOBAL)

execute_process(COMMAND "${IOSWITCH_PYTHON}" "${CMAKE_SOURCE_DIR}/cmake/package.py" --inputs
                OUTPUT_VARIABLE package_inputs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" package_inputs "${package_inputs}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/scripts/package_raw_boot.py" "${CMAKE_SOURCE_DIR}/cmake/package.py")

cmake_path(IS_PREFIX IOSWITCH_OUT "${CMAKE_BINARY_DIR}" NORMALIZE under_out)
if(NOT under_out)
  message(WARNING "The package target needs the build directory below ${IOSWITCH_OUT} "
                  "(scripts/build_native_loader.py writes its staging only there), e.g. -B out/cmake.")
endif()

set(smp "")
if(IOSWITCH_SMP)
  set(smp --smp)
endif()
add_custom_command(OUTPUT "${IOSWITCH_STAMPS}/package"
  COMMAND "${IOSWITCH_PYTHON}" "${CMAKE_SOURCE_DIR}/cmake/package.py" "${IOSWITCH_KERNEL_DIR}" "${IOSWITCH_BOOT_DIR}" ${smp}
  COMMAND "${CMAKE_COMMAND}" -E touch "${IOSWITCH_STAMPS}/package"
  DEPENDS "${IOSWITCH_KERNEL_DIR}/kernel.macho" "${CMAKE_SOURCE_DIR}/cmake/package.py" ${package_inputs}
  COMMENT "Packaging the boot entry into ${IOSWITCH_BOOT_DIR}"
  VERBATIM)
add_custom_target(package ALL DEPENDS "${IOSWITCH_STAMPS}/package")

# Never part of a plain build: writes the card at IOSWITCH_SD and ejects it.
add_custom_target(deploy
  COMMAND "${IOSWITCH_PYTHON}" "${CMAKE_SOURCE_DIR}/cmake/deploy.py" "${IOSWITCH_BOOT_DIR}" "${IOSWITCH_SD}"
  DEPENDS "${IOSWITCH_STAMPS}/package"
  COMMENT "Deploying ${IOSWITCH_BOOT_DIR} to ${IOSWITCH_SD}"
  USES_TERMINAL
  VERBATIM)
add_dependencies(deploy package)
