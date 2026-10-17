# SPDX-License-Identifier: Apache-2.0
# The options a builder sets. An enabled feature whose dependency is missing
# fails configuration rather than building without it; a CPU-only build turns
# LOTUS_ENABLE_VULKAN off (the `ci-core` intent does).
include_guard(GLOBAL)

option(LOTUS_ENABLE_VULKAN
  "Build the Vulkan backend and its shaders (needs a Vulkan 1.3 SDK with slangc)" ON)

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

foreach(_adapter IN ITEMS HYDRA2 VIEWPORT)
  if(LOTUS_ENABLE_${_adapter} AND NOT LOTUS_ENABLE_VULKAN)
    message(FATAL_ERROR "LOTUS_ENABLE_${_adapter} requires LOTUS_ENABLE_VULKAN")
  endif()
endforeach()

# Targets state their C++ standard with target_compile_features; this keeps
# the compiler's dialect strict wherever a target does not say otherwise.
set(CMAKE_CXX_EXTENSIONS OFF)
