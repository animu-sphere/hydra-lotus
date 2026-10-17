# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)

# VERSION is the project's version; release.py checks that openstrata.toml
# declares the same one.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LOTUS_VERSION)
string(STRIP "${LOTUS_VERSION}" LOTUS_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)

project(Lotus
  VERSION ${LOTUS_VERSION}
  DESCRIPTION "lotus — an OpenStrata renderer skeleton"
  LANGUAGES CXX)

# This file only orchestrates. Each directory owns its targets, finds its own
# dependencies and installs its own artifacts; the backend owns the shaders.
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(LotusOptions)
include(LotusWarnings)

if(LOTUS_BUILD_TESTS)
  include(CTest)
endif()

add_subdirectory(core/render-world)
add_subdirectory(core/render-extraction)
add_subdirectory(backend/vulkan)
add_subdirectory(adapters/headless)
if(LOTUS_ENABLE_HYDRA2)
  add_subdirectory(adapters/hydra2)
endif()
if(LOTUS_ENABLE_VIEWPORT)
  add_subdirectory(adapters/viewport)
endif()

if(LOTUS_BUILD_TESTS)
  add_subdirectory(validation)
endif()

include(LotusInstall)
