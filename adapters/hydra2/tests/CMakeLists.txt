# The Hydra adapter's CTests. The executables build beside hdLotus: the
# runtime they link finds the shaders hdLotus stages in its own directory.
get_filename_component(_lotus_hydra_binary_dir "${CMAKE_CURRENT_BINARY_DIR}" DIRECTORY)

# lotus_add_hydra_test(NAME <name> SOURCES <file>... [LIBRARIES <target>...])
#
# Adds the executable lotus-hydra2-<name>-test, built after hdLotus with the
# adapter's OpenUSD compile definitions.
function(lotus_add_hydra_test)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "SOURCES;LIBRARIES")
  set(target lotus-hydra2-${arg_NAME}-test)
  add_executable(${target} ${arg_SOURCES})
  target_link_libraries(${target} PRIVATE ${arg_LIBRARIES} Lotus::Warnings)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_lotus_hydra_binary_dir}")
  add_dependencies(${target} hdLotus)
endfunction()

set(_runtime lotus-hydra2-runtime)
# What a test that reads a USD stage links: it syncs one through UsdImaging.
set(_stage usdImaging usd sdf)

lotus_add_hydra_test(NAME render-buffer SOURCES render_buffer_test.cpp
  LIBRARIES ${_runtime})
lotus_add_hydra_test(NAME aov SOURCES aov_test.cpp
  LIBRARIES ${_runtime} Lotus::Vulkan)
lotus_add_hydra_test(NAME mesh SOURCES mesh_test.cpp
  LIBRARIES ${_runtime})
lotus_add_hydra_test(NAME instancer SOURCES instancer_test.cpp
  LIBRARIES ${_runtime} usdGeom ${_stage})
lotus_add_hydra_test(NAME render-pass SOURCES render_pass_test.cpp
  LIBRARIES ${_runtime} Lotus::Vulkan usdGeom ${_stage})
lotus_add_hydra_test(NAME material SOURCES material_test.cpp
  LIBRARIES ${_runtime} Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME normals SOURCES normals_test.cpp
  LIBRARIES ${_runtime} usdGeom ${_stage})
lotus_add_hydra_test(NAME texture SOURCES texture_test.cpp
  LIBRARIES ${_runtime} Lotus::Vulkan ${_stage} hio)
lotus_add_hydra_test(NAME light SOURCES light_test.cpp
  LIBRARIES ${_runtime} Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME deterministic SOURCES deterministic_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME integrator SOURCES integrator_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME temporal SOURCES temporal_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME restir SOURCES restir_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME light-sampling SOURCES light_sampling_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan ${_stage})
lotus_add_hydra_test(NAME analytic-light SOURCES analytic_light_test.cpp
  LIBRARIES ${_runtime} Lotus::RenderExtraction Lotus::Vulkan usdGeom ${_stage})
lotus_add_hydra_test(NAME dome-texture
  SOURCES dome_texture_test.cpp "${PROJECT_SOURCE_DIR}/adapters/headless/reference.cpp"
  LIBRARIES ${_runtime} Lotus::RenderWorld Lotus::Vulkan usdGeom ${_stage})
target_include_directories(lotus-hydra2-dome-texture-test
  PRIVATE "${PROJECT_SOURCE_DIR}/adapters/headless")
target_compile_definitions(lotus-hydra2-dome-texture-test PRIVATE
  LOTUS_ASSETS_DIRECTORY="${PROJECT_SOURCE_DIR}/validation/assets")
# The plugin probe loads hdLotus through plug, so it links no Lotus target.
add_executable(lotus-hydra2-probe plugin_probe.cpp)
target_link_libraries(lotus-hydra2-probe PRIVATE hd plug Lotus::Warnings)
target_compile_features(lotus-hydra2-probe PRIVATE cxx_std_20)
target_compile_definitions(lotus-hydra2-probe PRIVATE
  NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
add_dependencies(lotus-hydra2-probe hdLotus)

add_test(NAME lotus-renderer-plugin-discovery
  COMMAND lotus-hydra2-probe --discovery
    "$<TARGET_FILE_DIR:hdLotus>/resources")
add_test(NAME lotus-renderer-delegate-creation
  COMMAND lotus-hydra2-probe --delegate
    "$<TARGET_FILE_DIR:hdLotus>/resources")
add_test(NAME lotus-renderer-hydra-render-buffer
  COMMAND lotus-hydra2-render-buffer-test)
add_test(NAME lotus-renderer-hydra-aov COMMAND lotus-hydra2-aov-test)
add_test(NAME lotus-renderer-hydra-mesh COMMAND lotus-hydra2-mesh-test)
add_test(NAME lotus-renderer-hydra-instancer
  COMMAND lotus-hydra2-instancer-test)
add_test(NAME lotus-renderer-hydra-render-pass
  COMMAND lotus-hydra2-render-pass-test)
add_test(NAME lotus-renderer-hydra-render-pass-gpu
  COMMAND lotus-hydra2-render-pass-test --gpu)
add_test(NAME lotus-renderer-hydra-deterministic
  COMMAND lotus-hydra2-deterministic-test)
add_test(NAME lotus-renderer-hydra-integrator
  COMMAND lotus-hydra2-integrator-test)
add_test(NAME lotus-renderer-hydra-temporal-contract
  COMMAND lotus-hydra2-temporal-test --contract)
add_test(NAME lotus-renderer-hydra-temporal-gpu
  COMMAND lotus-hydra2-temporal-test --gpu)
add_test(NAME lotus-renderer-hydra-restir-contract
  COMMAND lotus-hydra2-restir-test --contract)
add_test(NAME lotus-renderer-hydra-restir-gpu
  COMMAND lotus-hydra2-restir-test --gpu)
add_test(NAME lotus-renderer-hydra-light-sampling
  COMMAND lotus-hydra2-light-sampling-test)
add_test(NAME lotus-renderer-hydra-light COMMAND lotus-hydra2-light-test)
add_test(NAME lotus-renderer-hydra-light-gpu
  COMMAND lotus-hydra2-light-test --gpu)
add_test(NAME lotus-renderer-hydra-dome-texture
  COMMAND lotus-hydra2-dome-texture-test)
add_test(NAME lotus-renderer-hydra-dome-texture-gpu
  COMMAND lotus-hydra2-dome-texture-test --gpu)
add_test(NAME lotus-renderer-hydra-analytic-lights
  COMMAND lotus-hydra2-analytic-light-test)
add_test(NAME lotus-renderer-hydra-analytic-lights-gpu
  COMMAND lotus-hydra2-analytic-light-test --gpu)
add_test(NAME lotus-renderer-hydra-material
  COMMAND lotus-hydra2-material-test)
add_test(NAME lotus-renderer-hydra-material-gpu
  COMMAND lotus-hydra2-material-test --gpu)
add_test(NAME lotus-renderer-hydra-normals
  COMMAND lotus-hydra2-normals-test)
add_test(NAME lotus-renderer-hydra-texture
  COMMAND lotus-hydra2-texture-test)
add_test(NAME lotus-renderer-hydra-texture-gpu
  COMMAND lotus-hydra2-texture-test --gpu)
set(_lotus_hydra_gpu_tests
  lotus-renderer-hydra-aov
  lotus-renderer-hydra-render-pass-gpu
  lotus-renderer-hydra-deterministic
  lotus-renderer-hydra-integrator
  lotus-renderer-hydra-temporal-gpu
  lotus-renderer-hydra-restir-gpu
  lotus-renderer-hydra-light-sampling
  lotus-renderer-hydra-light-gpu
  lotus-renderer-hydra-analytic-lights-gpu
  lotus-renderer-hydra-dome-texture-gpu
  lotus-renderer-hydra-material-gpu
  lotus-renderer-hydra-texture-gpu)
set_tests_properties(${_lotus_hydra_gpu_tests} PROPERTIES SKIP_RETURN_CODE 77)
# Every test above loads the OpenUSD runtime's DLLs; the rest need no device.
get_directory_property(_lotus_hydra_tests TESTS)
set_tests_properties(${_lotus_hydra_tests} PROPERTIES LABELS hydra
  ENVIRONMENT_MODIFICATION
  "PATH=path_list_prepend:${LOTUS_PXR_ROOT}/bin;PATH=path_list_prepend:${LOTUS_PXR_ROOT}/lib")
set(_lotus_hydra_fast_tests ${_lotus_hydra_tests})
list(REMOVE_ITEM _lotus_hydra_fast_tests ${_lotus_hydra_gpu_tests})
lotus_test_tier(gpu ${_lotus_hydra_gpu_tests})
lotus_test_tier(fast ${_lotus_hydra_fast_tests})

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(LOTUS_TESTUSDVIEW testusdview
  PATHS "${LOTUS_PXR_ROOT}/bin" NO_DEFAULT_PATH)
set(_lotus_has_display TRUE)
if(UNIX AND NOT APPLE AND NOT DEFINED ENV{DISPLAY} AND
   NOT DEFINED ENV{WAYLAND_DISPLAY})
  set(_lotus_has_display FALSE)
endif()
if(Python3_Interpreter_FOUND AND LOTUS_TESTUSDVIEW AND
   _lotus_has_display)
  add_test(NAME lotus-renderer-usdview-host
    COMMAND "${CMAKE_COMMAND}"
      "-DRENDERER_BUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DRENDERER_CONFIG=$<CONFIG>"
      "-DRENDERER_STAGE_DIR=${_lotus_hydra_binary_dir}/usdview-install"
      "-DRENDERER_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DRENDERER_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
      "-DRENDERER_PXR_ROOT=${LOTUS_PXR_ROOT}"
      "-DRENDERER_PYTHON=${Python3_EXECUTABLE}"
      "-DRENDERER_TESTUSDVIEW=${LOTUS_TESTUSDVIEW}"
      "-DRENDERER_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/usdview_smoke_test.py"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_usdview_smoke.cmake")
  set_tests_properties(lotus-renderer-usdview-host PROPERTIES
    LABELS "host-smoke;hydra"
    SKIP_REGULAR_EXPRESSION "SKIP usdview smoke:"
    TIMEOUT 210
    RUN_SERIAL TRUE)
  lotus_test_tier(gpu lotus-renderer-usdview-host)
else()
  message(STATUS
    "testusdview, matching Python, or a display is unavailable; usdview host evidence will be SKIP")
endif()
