# The viewport is an optional native host, not part of the renderer's
# validation contract: composition evidence stays owned by the headless
# adapter. GLFW builds C sources when fetched.
enable_language(C)
include(LotusShaders)

# The window layer creates its Vulkan surface itself.
find_package(Vulkan 1.3 REQUIRED)

# GLFW is the native window boundary. Prefer an installed package; otherwise
# fetch the pinned 3.4 release so every build compiles the same sources.
find_package(glfw3 3.4 CONFIG QUIET)
if(NOT TARGET glfw)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 7b6aead9fb88b3623e3b3725ebb42670cbe4c579 # 3.4
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(glfw)
endif()

# The built-in scene is the headless adapter's reference Cornell box, which
# has no dependency beyond the core.
add_executable(lotus-viewport
  builtin_scene.cpp
  frame_loop.cpp
  main.cpp
  options.cpp
  orbit_camera.cpp
  window_glfw.cpp
  "${PROJECT_SOURCE_DIR}/adapters/headless/reference.cpp")
target_include_directories(lotus-viewport PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/adapters/headless")
target_link_libraries(lotus-viewport PRIVATE
  Lotus::RenderWorld
  Lotus::RenderExtraction
  Lotus::Vulkan
  Vulkan::Vulkan
  glfw
  Lotus::Warnings)
target_compile_features(lotus-viewport PRIVATE cxx_std_20)

# A Hydra-enabled build opens USD stages: it drives the co-built Hydra
# runtime in process (project layout section 4, rule 4; DES-Q8).
if(LOTUS_ENABLE_HYDRA2)
  target_sources(lotus-viewport PRIVATE hydra_stage.cpp)
  target_link_libraries(lotus-viewport PRIVATE
    lotus-hydra2-runtime
    lotus-hydra2-host-dependencies)
  get_target_property(_pxr_root lotus-hydra2-host-dependencies LOTUS_PXR_ROOT)
  target_compile_definitions(lotus-viewport PRIVATE
    LOTUS_VIEWPORT_HYDRA=1
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  if(WIN32)
    # The OpenUSD runtime is an external SDK. The launcher beside each
    # configuration's executable puts its DLLs on PATH for that process
    # only, rather than requiring a machine-wide PATH change.
    file(GENERATE
      OUTPUT "$<TARGET_FILE_DIR:lotus-viewport>/run-lotus-viewport.cmd"
      CONTENT
        "@echo off\r\nsetlocal\r\nset \"PATH=${_pxr_root}/bin;${_pxr_root}/lib;%PATH%\"\r\n\"$<TARGET_FILE:lotus-viewport>\" %*\r\nexit /b %ERRORLEVEL%\r\n"
      TARGET lotus-viewport)
  endif()
endif()

lotus_stage_shaders(TARGET lotus-viewport)

# The installed tree reuses the bin/shaders the backend installs.
install(TARGETS lotus-viewport
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(LOTUS_BUILD_TESTS)
  set(_lotus_viewport_smoke
    --hidden --frames 8 --vsync off --validate --resize-test
    --allow-unavailable)
  add_test(NAME lotus-viewport-present
    COMMAND lotus-viewport ${_lotus_viewport_smoke}
      --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-benchmark.json")
  # Scripted navigation and scene edits, each of which must restart (or
  # continue) the accumulation.
  set(_lotus_viewport_interaction
    --hidden --frames 16 --vsync off --validate --interaction-test
    --allow-unavailable)
  add_test(NAME lotus-viewport-interaction
    COMMAND lotus-viewport ${_lotus_viewport_interaction})
  # Without a Vulkan driver the viewport explains the skip (77).
  add_test(NAME lotus-viewport-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-viewport>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/viewport-no-driver"
      "-DARGUMENTS=--hidden;--frames;2;--allow-unavailable"
      "-DEXPECTED=SKIP: .*no Vulkan driver presents"
      -P "${PROJECT_SOURCE_DIR}/validation/synchronization-no-driver-test.cmake")
  set(_lotus_viewport_gpu_tests lotus-viewport-present
    lotus-viewport-interaction)
  if(LOTUS_ENABLE_HYDRA2)
    set(_lotus_viewport_stage
      "${PROJECT_SOURCE_DIR}/adapters/hydra2/tests/usdview-smoke.usda")
    add_test(NAME lotus-viewport-hydra
      COMMAND lotus-viewport ${_lotus_viewport_smoke}
        --usd "${_lotus_viewport_stage}"
        --benchmark "${CMAKE_CURRENT_BINARY_DIR}/viewport-hydra-benchmark.json")
    add_test(NAME lotus-viewport-hydra-interaction
      COMMAND lotus-viewport ${_lotus_viewport_interaction}
        --usd "${_lotus_viewport_stage}")
    # Starts looking through the stage's authored camera at a time code.
    add_test(NAME lotus-viewport-hydra-camera
      COMMAND lotus-viewport --hidden --frames 4 --vsync off --validate
        --allow-unavailable --usd "${_lotus_viewport_stage}"
        --camera /Camera --time 1)
    list(APPEND _lotus_viewport_gpu_tests lotus-viewport-hydra
      lotus-viewport-hydra-interaction lotus-viewport-hydra-camera)
    set_tests_properties(${_lotus_viewport_gpu_tests}
      lotus-viewport-no-driver PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_pxr_root}/bin;PATH=path_list_prepend:${_pxr_root}/lib")
  endif()
  # Presentation shares the GPU and the desktop; keep it serial.
  set_tests_properties(${_lotus_viewport_gpu_tests} PROPERTIES
    LABELS "gpu;viewport;presentation"
    SKIP_RETURN_CODE 77
    RUN_SERIAL TRUE
    TIMEOUT 120)
  set_tests_properties(lotus-viewport-no-driver PROPERTIES
    LABELS "viewport;presentation")
endif()
