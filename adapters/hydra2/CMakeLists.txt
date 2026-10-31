include(LotusShaders)

find_package(pxr CONFIG REQUIRED)
if(NOT TARGET hd OR NOT TARGET hio OR NOT TARGET plug)
  message(FATAL_ERROR
    "The OpenUSD package must provide the hd, hio and plug CMake targets")
endif()

# The OpenUSD runtime root, whose bin/ and lib/ hold the DLLs that tests and
# the viewport launcher put on PATH.
if(PXR_INCLUDE_DIRS)
  list(GET PXR_INCLUDE_DIRS 0 _lotus_pxr_include)
  get_filename_component(LOTUS_PXR_ROOT
    "${_lotus_pxr_include}/.." ABSOLUTE)
elseif(DEFINED pxr_DIR)
  set(LOTUS_PXR_ROOT "${pxr_DIR}")
else()
  message(FATAL_ERROR "Could not derive the OpenUSD runtime root")
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
    Lotus::Warnings
    hio
    ${CMAKE_DL_LIBS})
target_compile_features(lotus-hydra2-runtime PUBLIC cxx_std_20)
target_compile_definitions(lotus-hydra2-runtime PRIVATE
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)

# The host-side OpenUSD libraries that a Hydra-enabled viewport links to open
# a stage and drive the delegate in process (project layout section 4,
# rule 4). Nothing else may link them outside this directory. Its
# LOTUS_PXR_ROOT property is the OpenUSD runtime root such a host puts on PATH.
add_library(lotus-hydra2-host-dependencies INTERFACE)
target_link_libraries(lotus-hydra2-host-dependencies INTERFACE
  usd usdGeom usdImaging)
set_target_properties(lotus-hydra2-host-dependencies PROPERTIES
  LOTUS_PXR_ROOT "${LOTUS_PXR_ROOT}")

add_library(hdLotus MODULE
  src/renderer_plugin.cpp
  resources/plugInfo.json.in)
target_link_libraries(hdLotus PRIVATE lotus-hydra2-runtime hd Lotus::Warnings)
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

# Plugin resources: plugInfo.json, the fallback environment with its licence
# and the backend's shaders beside the module.
set(LOTUS_HYDRA_LIBRARY_NAME
    "hdLotus${CMAKE_SHARED_MODULE_SUFFIX}")
configure_file(resources/plugInfo.json.in
  "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json" @ONLY)
set(LOTUS_HYDRA_ASSETS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/StinsonBeach.hdr"
  "${PROJECT_SOURCE_DIR}/third_party/openusd/LICENSE.txt"
  "${PROJECT_SOURCE_DIR}/third_party/openusd/NOTICE.txt")
add_custom_command(TARGET hdLotus POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:hdLotus>/resources"
    "$<TARGET_FILE_DIR:hdLotus>/assets"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
    "$<TARGET_FILE_DIR:hdLotus>/resources/plugInfo.json"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LOTUS_HYDRA_ASSETS}
    "$<TARGET_FILE_DIR:hdLotus>/assets"
  VERBATIM)
lotus_stage_shaders(TARGET hdLotus)

# Plugin install.
set(LOTUS_HYDRA_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/usd/hdLotus")
install(TARGETS hdLotus
  RUNTIME DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}"
  LIBRARY DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
  DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}/resources")
install(FILES ${LOTUS_HYDRA_ASSETS}
  DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}/assets")
lotus_install_shaders(DESTINATION "${LOTUS_HYDRA_INSTALL_DIR}/shaders")
# The stage the installed-tree usdview smoke opens.
install(FILES tests/usdview-smoke.usda
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lotus/tests")

if(LOTUS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
