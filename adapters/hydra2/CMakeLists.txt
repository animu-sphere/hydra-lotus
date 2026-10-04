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
add_library(lotus-hydra2-runtime STATIC src/adapter.cpp)
target_include_directories(lotus-hydra2-runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lotus-hydra2-runtime
  PUBLIC hd
  PRIVATE
    Lotus::RenderWorld
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
add_custom_command(TARGET hdLotus POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:hdLotus>/resources"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
    "$<TARGET_FILE_DIR:hdLotus>/resources/plugInfo.json"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
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
  set_tests_properties(
    lotus-renderer-plugin-discovery
    lotus-renderer-delegate-creation
    lotus-renderer-hydra-render-buffer
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
      TIMEOUT 60
      RUN_SERIAL TRUE)
  else()
    message(STATUS
      "testusdview, matching Python, or a display is unavailable; usdview host evidence will be SKIP")
  endif()
endif()
