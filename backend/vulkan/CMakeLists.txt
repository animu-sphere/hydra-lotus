add_library(lotus-render-vulkan STATIC
  reservoir.cpp
  shaders.cpp
  temporal.cpp
  temporal_metadata.cpp
  vulkan_backend.cpp)
add_library(Lotus::Vulkan ALIAS lotus-render-vulkan)
set_target_properties(lotus-render-vulkan PROPERTIES EXPORT_NAME Vulkan)
target_include_directories(lotus-render-vulkan PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(lotus-render-vulkan
  PUBLIC Lotus::RenderExtraction
  PRIVATE "$<BUILD_INTERFACE:Lotus::Warnings>")
target_compile_features(lotus-render-vulkan PUBLIC cxx_std_20)

# Without the Vulkan SDK the library still builds, so its capability probe
# and renderer creation report Skip in a CPU-only configuration.
if(LOTUS_ENABLE_VULKAN)
  find_package(Vulkan 1.3 QUIET)
  if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
      "LOTUS_ENABLE_VULKAN requires a Vulkan 1.3 SDK; configure with "
      "LOTUS_ENABLE_VULKAN=OFF for a CPU-only build")
  endif()
  target_sources(lotus-render-vulkan PRIVATE gpu_scene.cpp gpu_memory.cpp
    presentation.cpp)
  target_link_libraries(lotus-render-vulkan PRIVATE Vulkan::Vulkan)
  target_compile_definitions(lotus-render-vulkan PRIVATE LOTUS_HAS_VULKAN=1)
  add_subdirectory(shaders)
endif()
