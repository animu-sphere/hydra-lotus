# SPDX-License-Identifier: Apache-2.0
# Slang shader build and deployment. The Vulkan backend declares its shaders
# and the lotus-shaders aggregate (backend/vulkan/shaders); hosts depend on
# that aggregate and stage or install it, and never name a shader file.
include_guard(GLOBAL)

# lotus_add_slang_shader(TARGET <name> SOURCE <file.slang>
#   ENTRIES <entry>:<stage>... [OPTIONS <slangc argument>...]
#   [DEPENDS <imported module>...])
#
# Compiles each entry point of SOURCE, relative to the calling directory, to
# <source stem>.<vert|frag|comp>.spv in the calling directory's binary
# directory. slangc names every SPIR-V entry point `main`, which is what
# pipeline stage creation expects; modules import each other by their path
# below the calling directory. TARGET builds the outputs and lists them in its
# LOTUS_SHADER_OUTPUTS property.
function(lotus_add_slang_shader)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET;SOURCE"
    "ENTRIES;OPTIONS;DEPENDS")
  if(NOT arg_TARGET OR NOT arg_SOURCE OR NOT arg_ENTRIES)
    message(FATAL_ERROR
      "lotus_add_slang_shader needs TARGET, SOURCE and ENTRIES")
  endif()
  get_filename_component(stem "${arg_SOURCE}" NAME_WE)
  set(outputs "")
  foreach(entry IN LISTS arg_ENTRIES)
    string(REPLACE ":" ";" entry "${entry}")
    list(GET entry 0 entry_point)
    list(GET entry 1 stage)
    if(stage STREQUAL "vertex")
      set(suffix vert)
    elseif(stage STREQUAL "fragment")
      set(suffix frag)
    elseif(stage STREQUAL "compute")
      set(suffix comp)
    else()
      message(FATAL_ERROR "Unknown Slang stage '${stage}' in ${arg_TARGET}")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${stem}.${suffix}.spv")
    # The explicit matrix layout keeps host-visible data layouts
    # deterministic once shaders grow parameters.
    add_custom_command(
      OUTPUT "${output}"
      COMMAND "${LOTUS_SLANGC_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${arg_SOURCE}"
        -I "${CMAKE_CURRENT_SOURCE_DIR}"
        -entry "${entry_point}" -stage "${stage}" -target spirv
        ${arg_OPTIONS}
        -matrix-layout-column-major -warnings-as-errors all
        -o "${output}"
      DEPENDS "${arg_SOURCE}" ${arg_DEPENDS}
      VERBATIM)
    list(APPEND outputs "${output}")
  endforeach()
  add_custom_target(${arg_TARGET} DEPENDS ${outputs})
  set_property(TARGET ${arg_TARGET} PROPERTY LOTUS_SHADER_OUTPUTS ${outputs})
endfunction()

# lotus_stage_shaders(TARGET <host>)
#
# Builds lotus-shaders before <host> and copies them into shaders/ beside the
# host's binary after each build of it. A shader change relinks the host, so
# the copy follows it.
function(lotus_stage_shaders)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET" "")
  get_target_property(shaders lotus-shaders LOTUS_SHADER_OUTPUTS)
  add_dependencies(${arg_TARGET} lotus-shaders)
  set_property(TARGET ${arg_TARGET} APPEND PROPERTY LINK_DEPENDS ${shaders})
  add_custom_command(TARGET ${arg_TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:${arg_TARGET}>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${shaders}
      "$<TARGET_FILE_DIR:${arg_TARGET}>/shaders"
    VERBATIM)
endfunction()

# lotus_install_shaders(DESTINATION <directory>)
#
# Installs lotus-shaders into <directory>, the layout lotus_stage_shaders
# gives the build tree.
function(lotus_install_shaders)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "DESTINATION" "")
  get_target_property(shaders lotus-shaders LOTUS_SHADER_OUTPUTS)
  install(FILES ${shaders} DESTINATION "${arg_DESTINATION}")
endfunction()
