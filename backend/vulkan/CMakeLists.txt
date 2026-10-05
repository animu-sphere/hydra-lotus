add_library(lotus-render-vulkan STATIC
  vulkan_backend.cpp
  vulkan_present.cpp)
if(LOTUS_WITH_VULKAN)
  target_sources(lotus-render-vulkan PRIVATE gpu_scene.cpp)
endif()
add_library(Lotus::Vulkan ALIAS lotus-render-vulkan)
set_target_properties(lotus-render-vulkan PROPERTIES EXPORT_NAME Vulkan)
target_include_directories(lotus-render-vulkan PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(lotus-render-vulkan PUBLIC Lotus::RenderExtraction)
target_compile_features(lotus-render-vulkan PUBLIC cxx_std_20)
if(LOTUS_WITH_VULKAN)
  target_link_libraries(lotus-render-vulkan PRIVATE Vulkan::Vulkan)
  target_compile_definitions(lotus-render-vulkan PRIVATE LOTUS_HAS_VULKAN=1)

  # Slang is the single shader source. Each entry point compiles to its own
  # SPIR-V artifact; slangc names the SPIR-V entry point `main`, which is what
  # the pipeline stage creation expects. The explicit matrix layout keeps
  # host-visible data layouts deterministic once shaders grow parameters.
  set(_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.slang")
  set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(LOTUS_VERTEX_SPV "${_shader_dir}/triangle.vert.spv")
  set(LOTUS_FRAGMENT_SPV "${_shader_dir}/triangle.frag.spv")
  set(LOTUS_RAY_VERTEX_SPV "${_shader_dir}/primary_ray.vert.spv")
  set(LOTUS_RAY_FRAGMENT_SPV "${_shader_dir}/primary_ray.frag.spv")
  foreach(_stage IN ITEMS vertex fragment)
    if(_stage STREQUAL "vertex")
      set(_ray_output "${LOTUS_RAY_VERTEX_SPV}")
    else()
      set(_ray_output "${LOTUS_RAY_FRAGMENT_SPV}")
    endif()
    add_custom_command(
      OUTPUT "${_ray_output}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
      COMMAND "${LOTUS_SLANGC_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/primary_ray.slang"
        -entry "${_stage}_main" -stage "${_stage}" -target spirv
        -profile spirv_1_4 -capability spvRayQueryKHR
        -matrix-layout-column-major -warnings-as-errors all
        -o "${_ray_output}"
      DEPENDS shaders/primary_ray.slang
      VERBATIM)
  endforeach()
  add_custom_command(
    OUTPUT "${LOTUS_VERTEX_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
    COMMAND "${LOTUS_SLANGC_EXECUTABLE}" "${_shader_source}"
      -entry vertex_main -stage vertex -target spirv
      -matrix-layout-column-major -warnings-as-errors all
      -o "${LOTUS_VERTEX_SPV}"
    DEPENDS shaders/triangle.slang
    VERBATIM)
  add_custom_command(
    OUTPUT "${LOTUS_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
    COMMAND "${LOTUS_SLANGC_EXECUTABLE}" "${_shader_source}"
      -entry fragment_main -stage fragment -target spirv
      -matrix-layout-column-major -warnings-as-errors all
      -o "${LOTUS_FRAGMENT_SPV}"
    DEPENDS shaders/triangle.slang
    VERBATIM)
  add_custom_target(lotus-renderer-shaders
    DEPENDS "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}"
      "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}")
  set(LOTUS_VERTEX_SPV "${LOTUS_VERTEX_SPV}" PARENT_SCOPE)
  set(LOTUS_FRAGMENT_SPV "${LOTUS_FRAGMENT_SPV}" PARENT_SCOPE)
  set(LOTUS_RAY_VERTEX_SPV "${LOTUS_RAY_VERTEX_SPV}" PARENT_SCOPE)
  set(LOTUS_RAY_FRAGMENT_SPV "${LOTUS_RAY_FRAGMENT_SPV}" PARENT_SCOPE)
endif()
