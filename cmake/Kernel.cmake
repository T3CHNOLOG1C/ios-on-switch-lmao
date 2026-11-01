# The XNU build (the tree's own script and make) and the prelink of Apple's original kexts.
include_guard(GLOBAL)

set(IOSWITCH_KERNEL_DIR "${CMAKE_BINARY_DIR}/kernel")
set(IOSWITCH_XNU_KERNEL "${IOSWITCH_XNU_OBJ}/mach.development.bcm2837.unstripped.noctf")
set(IOSWITCH_STAMPS "${CMAKE_BINARY_DIR}/stamps")
file(MAKE_DIRECTORY "${IOSWITCH_KERNEL_DIR}" "${IOSWITCH_STAMPS}")

# XNU options: the pass's build-options.json, with its archive paths moved to IOSWITCH_TREE.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${IOSWITCH_XNU_OPTIONS}")
file(READ "${IOSWITCH_XNU_OPTIONS}" json)
string(JSON count LENGTH "${json}")
math(EXPR last "${count} - 1")
set(IOSWITCH_XNU_ENV "")
foreach(i RANGE ${last})
  string(JSON key MEMBER "${json}" ${i})
  string(JSON value GET "${json}" "${key}")
  string(REPLACE "${IOSWITCH_OPTIONS_TREE}" "${IOSWITCH_TREE}" value "${value}")
  list(APPEND IOSWITCH_XNU_ENV "${key}=${value}")
endforeach()

# make relinks when an archive in XNU_EXTERNAL_KERNEL_ARCHIVES changes, but not for objects
# or archives passed only on the link line (NativeRamBlock.o, XNU_EXTRA_LD_ARCHIVES). Remove
# the linked kernel whenever anything built here changes; nothing else forces a relink.
get_property(link_inputs GLOBAL PROPERTY IOSWITCH_LINK_INPUTS)
add_custom_command(OUTPUT "${IOSWITCH_STAMPS}/xnu-link-inputs"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${IOSWITCH_XNU_KERNEL}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${IOSWITCH_STAMPS}/xnu-link-inputs"
  DEPENDS ${link_inputs}
  COMMENT "Kernel link inputs changed"
  VERBATIM)

# XNU's make tracks its own sources, so it runs on every build; when it leaves the kernel
# untouched, ninja (restat) skips the prelink.
add_custom_target(xnu
  COMMAND "${CMAKE_SOURCE_DIR}/cmake/xnu-make.sh" "${IOSWITCH_TOOL_PATH}"
          "${IOSWITCH_TREE}/${IOSWITCH_XNU_SCRIPT}" ${IOSWITCH_XNU_ENV}
  BYPRODUCTS "${IOSWITCH_XNU_KERNEL}"
  DEPENDS "${IOSWITCH_STAMPS}/xnu-link-inputs"
  WORKING_DIRECTORY "${IOSWITCH_TREE}"
  COMMENT "Building XNU (${IOSWITCH_XNU_SCRIPT})"
  USES_TERMINAL
  VERBATIM)

# Prelink: the pass's prelink-command.json with this build's kernel and output.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${IOSWITCH_PASS}/prelink-command.json")
file(READ "${IOSWITCH_PASS}/prelink-command.json" json)
string(JSON count LENGTH "${json}")
math(EXPR last "${count} - 1")
set(prelink "")
set(replace "")
foreach(i RANGE ${last})
  string(JSON value GET "${json}" ${i})
  if(i EQUAL 1)
    set(value "${IOSWITCH_PRELINK_TOOL}")
  elseif(replace STREQUAL "--kernel")
    set(value "${IOSWITCH_KERNEL_DIR}/source-kernel.macho")
  elseif(replace STREQUAL "--output")
    set(value "${IOSWITCH_KERNEL_DIR}/kernel.macho")
  elseif(replace STREQUAL "--prelinked")
    set(kernelcache "${value}")
  endif()
  set(replace "${value}")
  list(APPEND prelink "${value}")
endforeach()
get_filename_component(prelink_dir "${IOSWITCH_PRELINK_TOOL}" DIRECTORY)

add_custom_command(
  OUTPUT "${IOSWITCH_KERNEL_DIR}/kernel.macho" "${IOSWITCH_KERNEL_DIR}/kernel.json"
         "${IOSWITCH_KERNEL_DIR}/source-kernel.macho"
  # emit_prelink.py refuses to overwrite its output.
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${IOSWITCH_KERNEL_DIR}/kernel.macho" "${IOSWITCH_KERNEL_DIR}/kernel.json"
  COMMAND "${CMAKE_COMMAND}" -E copy "${IOSWITCH_XNU_KERNEL}" "${IOSWITCH_KERNEL_DIR}/source-kernel.macho"
  COMMAND ${prelink}
  DEPENDS "${IOSWITCH_XNU_KERNEL}" "${IOSWITCH_PRELINK_TOOL}" "${prelink_dir}/native_prelink.py" "${kernelcache}"
  COMMENT "Prelinking the original kexts into kernel.macho"
  VERBATIM)
add_custom_target(kernel ALL DEPENDS "${IOSWITCH_KERNEL_DIR}/kernel.macho")
add_dependencies(kernel xnu)
