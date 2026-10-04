# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)

project(Lotus
  VERSION 0.1.0
  DESCRIPTION "lotus — an OpenStrata renderer skeleton"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LOTUS_ENABLE_VULKAN "Probe and link the Vulkan backend capability" ON)

# Workflow requests add adapters to explicit project options. An omitted adapter
# never disables another option from a project-declared intent.
set(OST_RENDERER_ADAPTERS "" CACHE STRING
  "Semicolon-separated additional renderer adapters requested by OpenStrata")
option(LOTUS_ENABLE_HYDRA2 "Build the optional co-built OpenUSD Hydra 2 adapter" OFF)
option(LOTUS_ENABLE_VIEWPORT "Build the optional GLFW/Vulkan viewport adapter" OFF)
if("hydra2" IN_LIST OST_RENDERER_ADAPTERS)
  set(LOTUS_ENABLE_HYDRA2 ON CACHE BOOL
    "Build the optional co-built OpenUSD Hydra 2 adapter" FORCE)
endif()
if("viewport" IN_LIST OST_RENDERER_ADAPTERS)
  set(LOTUS_ENABLE_VIEWPORT ON CACHE BOOL
    "Build the optional GLFW/Vulkan viewport adapter" FORCE)
endif()
option(LOTUS_BUILD_TESTS "Build renderer contract and install-tree tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LOTUS_BUILD_TESTS)
  include(CTest)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(LOTUS_WITH_VULKAN OFF)
if(LOTUS_ENABLE_VULKAN)
  find_package(Vulkan 1.3 QUIET)
  # Slang is the shader source of truth. The Vulkan SDK bundles slangc since
  # 1.3.296, so one SDK install provides both the loader and the compiler.
  find_program(LOTUS_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES bin Bin)
  if(Vulkan_FOUND AND LOTUS_SLANGC_EXECUTABLE)
    set(LOTUS_WITH_VULKAN ON)
  elseif(NOT Vulkan_FOUND)
    message(STATUS "Vulkan 1.3 was not found; GPU assertions will report SKIP")
  else()
    message(STATUS
      "slangc was not found (the Vulkan SDK bundles it since 1.3.296); "
      "GPU assertions will report SKIP")
  endif()
endif()

add_subdirectory(core/render-world)
add_subdirectory(core/render-extraction)
add_subdirectory(backend/vulkan)
add_subdirectory(adapters/headless)
if(LOTUS_ENABLE_HYDRA2)
  add_subdirectory(adapters/hydra2)
  target_compile_definitions(lotus-headless PRIVATE LOTUS_HAS_HYDRA2=1)
endif()
if(LOTUS_ENABLE_VIEWPORT)
  add_subdirectory(adapters/viewport)
endif()

if(LOTUS_BUILD_TESTS)
  add_subdirectory(validation)
endif()

# One project-level install graph. Internal source packs are CMake targets, not
# independently versioned packages.
install(TARGETS
    lotus-render-world
    lotus-render-extraction
    lotus-render-vulkan
  EXPORT LotusTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS lotus-headless
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(LOTUS_WITH_VULKAN)
  install(FILES "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders")
endif()
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(Lotus_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Lotus")
configure_package_config_file(
  cmake/LotusConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/LotusConfig.cmake"
  INSTALL_DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LotusConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(EXPORT LotusTargets
  FILE LotusTargets.cmake
  NAMESPACE Lotus::
  DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LotusConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LotusConfigVersion.cmake"
  DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")
install(FILES openstrata.renderer.yaml openstrata.scaffold.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lotus")
