add_library(lotus-render-vulkan STATIC
  vulkan_backend.cpp
  vulkan_present.cpp)
if(LOTUS_WITH_VULKAN)
  target_sources(lotus-render-vulkan PRIVATE gpu_scene.cpp gpu_memory.cpp)
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
  set(LOTUS_RAY_VERTEX_SPV "${_shader_dir}/path_trace.vert.spv")
  set(LOTUS_RAY_FRAGMENT_SPV "${_shader_dir}/path_trace.frag.spv")
  set(LOTUS_WAVEFRONT_SPV "${_shader_dir}/wavefront.comp.spv")
  # The path tracer's entry points import these modules, by their path
  # below shaders/; so do the wavefront kernels.
  set(_scene_modules
    shaders/bsdf/bsdf.slang
    shaders/common/scene.slang
    shaders/common/spectrum.slang
    shaders/common/transport.slang
    shaders/sampling/sampling.slang)
  set(_path_trace_sources shaders/path_trace.slang ${_scene_modules})
  # Texture sampling needs Slang's sm_5_0 capability set and the texture
  # table's non-uniform indexing spvShaderNonUniformEXT; declaring them keeps
  # slangc from upgrading the profile implicitly, which it warns about. The
  # SPIR-V still declares only the capabilities the shader uses.
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
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/path_trace.slang"
        -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        -entry "${_stage}_main" -stage "${_stage}" -target spirv
        -profile spirv_1_4 -capability spvRayQueryKHR
        -capability spvShaderNonUniformEXT -capability sm_5_0
        -matrix-layout-column-major -warnings-as-errors all
        -o "${_ray_output}"
      DEPENDS ${_path_trace_sources}
      VERBATIM)
  endforeach()
  # The wavefront kernels: one compute entry point, specialized per kernel.
  add_custom_command(
    OUTPUT "${LOTUS_WAVEFRONT_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
    COMMAND "${LOTUS_SLANGC_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wavefront/wavefront.slang"
      -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
      -entry wavefront_main -stage compute -target spirv
      -profile spirv_1_4 -capability spvRayQueryKHR
      -capability spvShaderNonUniformEXT -capability sm_5_0
      -matrix-layout-column-major -warnings-as-errors all
      -o "${LOTUS_WAVEFRONT_SPV}"
    DEPENDS shaders/wavefront/wavefront.slang ${_scene_modules}
    VERBATIM)
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
      "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
      "${LOTUS_WAVEFRONT_SPV}")
  set(LOTUS_VERTEX_SPV "${LOTUS_VERTEX_SPV}" PARENT_SCOPE)
  set(LOTUS_FRAGMENT_SPV "${LOTUS_FRAGMENT_SPV}" PARENT_SCOPE)
  set(LOTUS_RAY_VERTEX_SPV "${LOTUS_RAY_VERTEX_SPV}" PARENT_SCOPE)
  set(LOTUS_RAY_FRAGMENT_SPV "${LOTUS_RAY_FRAGMENT_SPV}" PARENT_SCOPE)
  set(LOTUS_WAVEFRONT_SPV "${LOTUS_WAVEFRONT_SPV}" PARENT_SCOPE)
endif()
