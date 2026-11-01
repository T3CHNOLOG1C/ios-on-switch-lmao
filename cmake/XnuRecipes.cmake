# Compile recipes for code linked into the kernel, taken from XNU's own per-object compile
# records (the clang -MJ entries XNU's make leaves next to every object), the same way the
# tree's scripts/build-*.py builders derived them. Every object is a custom command with a
# clang depfile, so header edits rebuild exactly what includes them.
include_guard(GLOBAL)

set(IOSWITCH_XNU_OBJ "${IOSWITCH_TREE}/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837")
set(IOSWITCH_RECIPES "${CMAKE_BINARY_DIR}/recipes")
set(IOSWITCH_DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${IOSWITCH_RECIPES}" "${IOSWITCH_DEPS}")

execute_process(COMMAND xcrun --sdk iphoneos --show-sdk-path
                OUTPUT_VARIABLE IOSWITCH_IPHONEOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)

# ioswitch_recipe(<name> RECORD <path below the XNU objdir> [KEEP_WARNINGS] [DROP <prefix>...])
# Writes recipes/<name>.rsp: the record's arguments without its compiler, source, -o and
# dependency outputs, and (unless KEEP_WARNINGS) without its -W flags; arguments starting
# with a DROP prefix are left out too. `-I .` and `-include meta_features.h` are made
# absolute so clang's depfile never names a path relative to the record directory, where
# the compile still runs (it is the debug info's comp_dir, as with the Python builders).
# Sets IOSWITCH_RECIPE_<name>_CC and IOSWITCH_RECIPE_<name>_DIR.
function(ioswitch_recipe name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "KEEP_WARNINGS" "RECORD" "DROP")
  set(record "${IOSWITCH_XNU_OBJ}/${arg_RECORD}")
  if(NOT EXISTS "${record}")
    message(FATAL_ERROR "${record} is missing. Compile recipes come from XNU's compile records: "
                        "build XNU in this tree once (XNU_BOOTSTRAP=1) before configuring.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${record}")
  file(READ "${record}" json)
  string(REGEX REPLACE ",[ \t\r\n]*$" "" json "${json}")   # records are array members
  string(JSON dir GET "${json}" directory)
  string(JSON source GET "${json}" file)
  string(JSON count LENGTH "${json}" arguments)
  # A record names the tree it was written in; point it at IOSWITCH_TREE.
  string(REGEX REPLACE "/build/xnu/ios-obj/.*$" "" recorded_tree "${dir}")
  string(REPLACE "${recorded_tree}" "${IOSWITCH_TREE}" dir "${dir}")
  string(JSON cc GET "${json}" arguments 0)

  set(content "")
  set(skip FALSE)
  set(previous "")
  math(EXPR last "${count} - 1")
  foreach(i RANGE 1 ${last})
    string(JSON value GET "${json}" arguments ${i})
    if(skip)
      set(skip FALSE)
      set(previous "")
      continue()
    endif()
    if(value STREQUAL source OR value MATCHES "^-M[DP]$")
      continue()
    endif()
    if(value MATCHES "^-(o|MF|MJ)$")
      set(skip TRUE)
      continue()
    endif()
    if(NOT arg_KEEP_WARNINGS)
      if(value MATCHES "^-W")
        continue()
      endif()
      if(value STREQUAL "-Xclang")
        math(EXPR next "${i} + 1")
        string(JSON following GET "${json}" arguments ${next})
        if(following MATCHES "^-W")
          set(skip TRUE)
          continue()
        endif()
      endif()
    endif()
    set(dropped FALSE)
    foreach(prefix IN LISTS arg_DROP)
      string(FIND "${value}" "${prefix}" at)
      if(at EQUAL 0)
        set(dropped TRUE)
      endif()
    endforeach()
    if(dropped)
      continue()
    endif()
    if(previous STREQUAL "-I" AND value STREQUAL ".")
      set(value "${dir}")
    elseif(previous STREQUAL "-include" AND NOT IS_ABSOLUTE "${value}" AND EXISTS "${dir}/${value}")
      set(value "${dir}/${value}")
    endif()
    string(REPLACE "${recorded_tree}" "${IOSWITCH_TREE}" value "${value}")
    set(previous "${value}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(APPEND content "\"${value}\"\n")
  endforeach()

  # Rewrite the response file only when it changes, so reconfiguring rebuilds nothing.
  file(WRITE "${IOSWITCH_RECIPES}/${name}.rsp.new" "${content}")
  configure_file("${IOSWITCH_RECIPES}/${name}.rsp.new" "${IOSWITCH_RECIPES}/${name}.rsp" COPYONLY)
  set(IOSWITCH_RECIPE_${name}_CC "${cc}" PARENT_SCOPE)
  set(IOSWITCH_RECIPE_${name}_DIR "${dir}" PARENT_SCOPE)
endfunction()

# ioswitch_compile(<object> RECIPE <name> SOURCE <file> [FLAGS <arg>...])
# Compiles <file> with the recipe's arguments followed by FLAGS.
function(ioswitch_compile object)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "RECIPE;SOURCE" "FLAGS")
  if(NOT DEFINED IOSWITCH_RECIPE_${arg_RECIPE}_CC)
    message(FATAL_ERROR "Unknown compile recipe ${arg_RECIPE}")
  endif()
  _ioswitch_depfile(depfile "${object}")
  set(rsp "${IOSWITCH_RECIPES}/${arg_RECIPE}.rsp")
  file(RELATIVE_PATH shown "${IOSWITCH_TREE}" "${arg_SOURCE}")
  add_custom_command(OUTPUT "${object}"
    COMMAND "${IOSWITCH_RECIPE_${arg_RECIPE}_CC}" "@${rsp}" ${arg_FLAGS} -MD -MF "${depfile}"
            -o "${object}" "${arg_SOURCE}"
    DEPENDS "${arg_SOURCE}" "${rsp}"
    DEPFILE "${depfile}"
    WORKING_DIRECTORY "${IOSWITCH_RECIPE_${arg_RECIPE}_DIR}"
    COMMENT "Compiling ${shown}"
    VERBATIM)
endfunction()

# ioswitch_compile_command(<object> SOURCE <file> COMMAND <compiler and flags>...)
# For the few units that are not built with an XNU recipe.
function(ioswitch_compile_command object)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE" "COMMAND")
  _ioswitch_depfile(depfile "${object}")
  file(RELATIVE_PATH shown "${IOSWITCH_TREE}" "${arg_SOURCE}")
  add_custom_command(OUTPUT "${object}"
    COMMAND ${arg_COMMAND} -MD -MF "${depfile}" -c "${arg_SOURCE}" -o "${object}"
    DEPENDS "${arg_SOURCE}"
    DEPFILE "${depfile}"
    COMMENT "Compiling ${shown}"
    VERBATIM)
endfunction()

# ioswitch_bindings(<object> <file.S>): absolute-symbol aliases into the original kernelcache.
function(ioswitch_bindings object source)
  ioswitch_compile_command("${object}" SOURCE "${source}"
    COMMAND xcrun clang -target arm64-apple-ios17.0 -isysroot "${IOSWITCH_IPHONEOS_SDK}" -Werror)
endfunction()

# ioswitch_late_object(<late.o> <object> <source file name>): drop the object's
# __mod_init_func and export its static initializer, which the board code runs once the
# original family it subclasses has loaded.
function(ioswitch_late_object late object source_name)
  add_custom_command(OUTPUT "${late}"
    COMMAND "${IOSWITCH_LLVM_OBJCOPY}" --remove-section=__DATA,__mod_init_func
            "--globalize-symbol=__GLOBAL__sub_I_${source_name}" "${object}" "${late}"
    DEPENDS "${object}"
    COMMENT "Staging the static initializer of ${source_name}"
    VERBATIM)
endfunction()

# ioswitch_archive(<archive> <member>...): a fresh archive, members in the given order
# (with -force_load the order is the link order).
function(ioswitch_archive archive)
  get_filename_component(shown "${archive}" NAME)
  add_custom_command(OUTPUT "${archive}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${archive}"
    COMMAND xcrun ar rcs "${archive}" ${ARGN}
    DEPENDS ${ARGN}
    COMMENT "Archiving ${shown}"
    VERBATIM)
  set_property(GLOBAL APPEND PROPERTY IOSWITCH_LINK_INPUTS "${archive}")
endfunction()

function(_ioswitch_depfile out object)
  file(RELATIVE_PATH relative "${IOSWITCH_TREE}" "${object}")
  string(REPLACE "/" "_" relative "${relative}")
  get_filename_component(directory "${object}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  set(${out} "${IOSWITCH_DEPS}/${relative}.d" PARENT_SCOPE)
endfunction()
