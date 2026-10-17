# The backend's shader graph. Slang is the single shader source; hosts see
# only the lotus-shaders aggregate (cmake/LotusShaders.cmake).
include(LotusShaders)

# Slang is the shader source of truth. The Vulkan SDK bundles slangc since
# 1.3.296, so one SDK install provides both the loader and the compiler.
find_program(LOTUS_SLANGC_EXECUTABLE
  NAMES slangc
  HINTS ENV VULKAN_SDK
  PATH_SUFFIXES bin Bin)
if(NOT LOTUS_SLANGC_EXECUTABLE)
  message(FATAL_ERROR
    "LOTUS_ENABLE_VULKAN requires slangc (the Vulkan SDK bundles it since "
    "1.3.296); configure with LOTUS_ENABLE_VULKAN=OFF for a CPU-only build")
endif()

# The modules the path tracer's entry points and the wavefront kernels import.
set(_scene_modules
  bsdf/bsdf.slang
  common/path_state.slang
  common/restir.slang
  common/temporal.slang
  common/scene.slang
  common/spectrum.slang
  common/transport.slang
  sampling/sampling.slang
  sampling/reservoir.slang)
# Texture sampling needs Slang's sm_5_0 capability set and the texture
# table's non-uniform indexing spvShaderNonUniformEXT; declaring them keeps
# slangc from upgrading the profile implicitly, which it warns about. The
# SPIR-V still declares only the capabilities the shader uses.
set(_ray_query_options
  -profile spirv_1_4 -capability spvRayQueryKHR
  -capability spvShaderNonUniformEXT -capability sm_5_0)

lotus_add_slang_shader(
  TARGET lotus-shader-triangle
  SOURCE triangle.slang
  ENTRIES vertex_main:vertex fragment_main:fragment)

lotus_add_slang_shader(
  TARGET lotus-shader-pathtrace
  SOURCE path_trace.slang
  ENTRIES vertex_main:vertex fragment_main:fragment
  OPTIONS ${_ray_query_options}
  DEPENDS ${_scene_modules})

# The wavefront kernels: one compute entry point, specialized per kernel.
lotus_add_slang_shader(
  TARGET lotus-shader-wavefront
  SOURCE wavefront/wavefront.slang
  ENTRIES wavefront_main:compute
  OPTIONS ${_ray_query_options}
  DEPENDS ${_scene_modules})

# Present's debug views: the latest frame's debug products as display
# colours.
lotus_add_slang_shader(
  TARGET lotus-shader-present-view
  SOURCE present/present_view.slang
  ENTRIES present_main:compute)

# Every shader a renderer loads, in the one directory that ShadersIn
# (include/lotus/vulkan_backend.hpp) names the files in. Hosts stage or
# install LOTUS_SHADER_OUTPUTS; build-tree checks may read
# LOTUS_SHADER_DIRECTORY in place.
set(_shaders lotus-shader-triangle lotus-shader-pathtrace lotus-shader-wavefront
  lotus-shader-present-view)
add_custom_target(lotus-shaders)
add_dependencies(lotus-shaders ${_shaders})
foreach(_shader IN LISTS _shaders)
  get_target_property(_outputs ${_shader} LOTUS_SHADER_OUTPUTS)
  set_property(TARGET lotus-shaders APPEND PROPERTY
    LOTUS_SHADER_OUTPUTS ${_outputs})
endforeach()
set_property(TARGET lotus-shaders PROPERTY
  LOTUS_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

lotus_install_shaders(DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders")
