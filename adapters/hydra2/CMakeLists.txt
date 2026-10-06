find_package(pxr CONFIG REQUIRED)

if(NOT LOTUS_WITH_VULKAN)
  message(FATAL_ERROR
    "LOTUS_ENABLE_HYDRA2 requires a Vulkan 1.3 SDK and slangc")
endif()
if(NOT TARGET hd OR NOT TARGET plug)
  message(FATAL_ERROR
    "The OpenUSD package must provide the hd and plug CMake targets")
endif()

# The co-built runtime is implementation detail of the module. It is not an
# independently packaged OpenStrata plugin bundle or renderer SDK component.
add_library(lotus-hydra2-runtime STATIC src/adapter.cpp
  src/material_translator.cpp)
target_include_directories(lotus-hydra2-runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lotus-hydra2-runtime
  PUBLIC hd Lotus::RenderWorld
  PRIVATE
    Lotus::RenderExtraction
    Lotus::Vulkan
    ${CMAKE_DL_LIBS})
target_compile_features(lotus-hydra2-runtime PUBLIC cxx_std_20)
target_compile_definitions(lotus-hydra2-runtime PRIVATE
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

add_library(hdLotus MODULE
  src/renderer_plugin.cpp
  resources/plugInfo.json.in)
target_link_libraries(hdLotus PRIVATE lotus-hydra2-runtime hd)
target_compile_features(hdLotus PRIVATE cxx_std_20)
target_compile_definitions(hdLotus PRIVATE
  NOMINMAX
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
set_target_properties(hdLotus PROPERTIES PREFIX "")

# Static renderer packs become part of the Hydra module on ELF/Mach-O hosts.
set_target_properties(
  lotus-render-world
  lotus-render-extraction
  lotus-render-vulkan
  lotus-hydra2-runtime
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(LOTUS_HYDRA_LIBRARY_NAME
    "hdLotus${CMAKE_SHARED_MODULE_SUFFIX}")
configure_file(resources/plugInfo.json.in
  "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json" @ONLY)

add_dependencies(hdLotus lotus-renderer-shaders)
# A shader change relinks, so the copies below follow it.
set_property(TARGET hdLotus APPEND PROPERTY LINK_DEPENDS
  "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
  "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}")
add_custom_command(TARGET hdLotus POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:hdLotus>/resources"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
    "$<TARGET_FILE_DIR:hdLotus>/resources/plugInfo.json"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:hdLotus>/shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
    "$<TARGET_FILE_DIR:hdLotus>/shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${LOTUS_VERTEX_SPV}"
    "$<TARGET_FILE_DIR:hdLotus>/shaders/triangle.vert.spv"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${LOTUS_FRAGMENT_SPV}"
    "$<TARGET_FILE_DIR:hdLotus>/shaders/triangle.frag.spv"
  VERBATIM)

set(LOTUS_HYDRA_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/usd/hdLotus")
install(TARGETS hdLotus
  RUNTIME DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}"
  LIBRARY DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
  DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}/resources")
install(FILES "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}"
  "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
  DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}/shaders")
install(FILES tests/usdview-smoke.usda
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lotus/tests")

if(LOTUS_BUILD_TESTS)
  add_executable(lotus-hydra2-probe tests/plugin_probe.cpp)
  target_link_libraries(lotus-hydra2-probe PRIVATE hd plug)
  target_compile_features(lotus-hydra2-probe PRIVATE cxx_std_20)
  target_compile_definitions(lotus-hydra2-probe PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_dependencies(lotus-hydra2-probe hdLotus)

  add_executable(lotus-hydra2-render-buffer-test
    tests/render_buffer_test.cpp)
  target_link_libraries(lotus-hydra2-render-buffer-test
    PRIVATE lotus-hydra2-runtime)
  target_compile_features(lotus-hydra2-render-buffer-test PRIVATE cxx_std_20)
  target_compile_definitions(lotus-hydra2-render-buffer-test PRIVATE
    NOMINMAX
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

  add_executable(lotus-hydra2-aov-test tests/aov_test.cpp)
  target_link_libraries(lotus-hydra2-aov-test
    PRIVATE lotus-hydra2-runtime Lotus::Vulkan)
  target_compile_definitions(lotus-hydra2-aov-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_dependencies(lotus-hydra2-aov-test hdLotus)

  add_executable(lotus-hydra2-mesh-test tests/mesh_test.cpp)
  target_link_libraries(lotus-hydra2-mesh-test PRIVATE lotus-hydra2-runtime)
  target_compile_definitions(lotus-hydra2-mesh-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

  # Only these tests read a USD stage: they sync one through UsdImaging.
  add_executable(lotus-hydra2-instancer-test tests/instancer_test.cpp)
  target_link_libraries(lotus-hydra2-instancer-test
    PRIVATE lotus-hydra2-runtime usdImaging usdGeom usd sdf)
  target_compile_definitions(lotus-hydra2-instancer-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

  add_executable(lotus-hydra2-render-pass-test tests/render_pass_test.cpp)
  target_link_libraries(lotus-hydra2-render-pass-test
    PRIVATE lotus-hydra2-runtime Lotus::Vulkan usdImaging usdGeom usd sdf)
  target_compile_definitions(lotus-hydra2-render-pass-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_dependencies(lotus-hydra2-render-pass-test hdLotus)

  add_executable(lotus-hydra2-material-test tests/material_test.cpp)
  target_link_libraries(lotus-hydra2-material-test
    PRIVATE lotus-hydra2-runtime Lotus::Vulkan usdImaging usd sdf)
  target_compile_definitions(lotus-hydra2-material-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_dependencies(lotus-hydra2-material-test hdLotus)

  add_executable(lotus-hydra2-normals-test tests/normals_test.cpp)
  target_link_libraries(lotus-hydra2-normals-test
    PRIVATE lotus-hydra2-runtime usdImaging usdGeom usd sdf)
  target_compile_definitions(lotus-hydra2-normals-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

  add_executable(lotus-hydra2-deterministic-test tests/deterministic_test.cpp)
  target_link_libraries(lotus-hydra2-deterministic-test
    PRIVATE lotus-hydra2-runtime Lotus::RenderExtraction Lotus::Vulkan
      usdImaging usd sdf)
  target_compile_definitions(lotus-hydra2-deterministic-test PRIVATE
    NOMINMAX _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  add_dependencies(lotus-hydra2-deterministic-test hdLotus)

  if(PXR_INCLUDE_DIRS)
    list(GET PXR_INCLUDE_DIRS 0 _lotus_pxr_include)
    get_filename_component(LOTUS_PXR_ROOT
      "${_lotus_pxr_include}/.." ABSOLUTE)
  elseif(DEFINED pxr_DIR)
    set(LOTUS_PXR_ROOT "${pxr_DIR}")
  else()
    message(FATAL_ERROR "Could not derive the OpenUSD runtime root")
  endif()

  set(_lotus_pxr_runtime_modifications
    "PATH=path_list_prepend:${LOTUS_PXR_ROOT}/bin"
    "PATH=path_list_prepend:${LOTUS_PXR_ROOT}/lib")

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
  add_test(NAME lotus-renderer-hydra-material
    COMMAND lotus-hydra2-material-test)
  add_test(NAME lotus-renderer-hydra-material-gpu
    COMMAND lotus-hydra2-material-test --gpu)
  add_test(NAME lotus-renderer-hydra-normals
    COMMAND lotus-hydra2-normals-test)
  set_tests_properties(
    lotus-renderer-hydra-aov
    lotus-renderer-hydra-render-pass-gpu
    lotus-renderer-hydra-deterministic
    lotus-renderer-hydra-material-gpu
    PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)
  set_tests_properties(
    lotus-renderer-plugin-discovery
    lotus-renderer-delegate-creation
    lotus-renderer-hydra-render-buffer
    lotus-renderer-hydra-aov
    lotus-renderer-hydra-mesh
    lotus-renderer-hydra-instancer
    lotus-renderer-hydra-render-pass
    lotus-renderer-hydra-render-pass-gpu
    lotus-renderer-hydra-deterministic
    lotus-renderer-hydra-material
    lotus-renderer-hydra-material-gpu
    lotus-renderer-hydra-normals
    PROPERTIES ENVIRONMENT_MODIFICATION
      "${_lotus_pxr_runtime_modifications}")

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
        "-DRENDERER_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/usdview-install"
        "-DRENDERER_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
        "-DRENDERER_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DRENDERER_PXR_ROOT=${LOTUS_PXR_ROOT}"
        "-DRENDERER_PYTHON=${Python3_EXECUTABLE}"
        "-DRENDERER_TESTUSDVIEW=${LOTUS_TESTUSDVIEW}"
        "-DRENDERER_TEST_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/usdview_smoke_test.py"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_usdview_smoke.cmake")
    set_tests_properties(lotus-renderer-usdview-host PROPERTIES
      LABELS "host-smoke;gpu"
      SKIP_REGULAR_EXPRESSION "SKIP usdview smoke:"
      TIMEOUT 60
      RUN_SERIAL TRUE)
  else()
    message(STATUS
      "testusdview, matching Python, or a display is unavailable; usdview host evidence will be SKIP")
  endif()
endif()
