# SPDX-License-Identifier: Apache-2.0
# The project's SDK install and package export. Internal source packs are CMake
# targets, not independently versioned packages. Components install their own
# artifacts: the backend its shaders, each adapter its binaries and resources.
include_guard(GLOBAL)

install(TARGETS
    lotus-render-world
    lotus-render-extraction
    lotus-render-vulkan
  EXPORT LotusTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(Lotus_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Lotus")
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/LotusConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/LotusConfig.cmake"
  INSTALL_DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/LotusConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(EXPORT LotusTargets
  FILE LotusTargets.cmake
  NAMESPACE Lotus::
  DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")
install(FILES
    "${PROJECT_BINARY_DIR}/LotusConfig.cmake"
    "${PROJECT_BINARY_DIR}/LotusConfigVersion.cmake"
  DESTINATION "${Lotus_CONFIG_INSTALL_DIR}")

# The OpenStrata renderer and scaffold descriptors.
install(FILES
    "${PROJECT_SOURCE_DIR}/openstrata.renderer.yaml"
    "${PROJECT_SOURCE_DIR}/openstrata.scaffold.yaml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lotus")
