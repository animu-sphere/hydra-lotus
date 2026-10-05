configure_file(
  run-install-smoke.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/run-install-smoke.cmake"
  @ONLY)
configure_file(
  check-core-boundary.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/check-core-boundary.cmake"
  @ONLY)

add_test(NAME lotus-renderer-core-boundary
  COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_BINARY_DIR}/check-core-boundary.cmake")

add_test(NAME lotus-renderer-core-boundary-discovery
  COMMAND "${CMAKE_COMMAND}"
    "-DCHECKER=${CMAKE_CURRENT_BINARY_DIR}/check-core-boundary.cmake"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/core-boundary-test.cmake")

add_executable(lotus-viewport-camera-test viewport-camera-test.cpp)
target_link_libraries(lotus-viewport-camera-test PRIVATE Lotus::RenderExtraction)
add_test(NAME lotus-viewport-camera COMMAND lotus-viewport-camera-test)

add_executable(lotus-render-world-mesh-test render-world-mesh-test.cpp)
target_link_libraries(lotus-render-world-mesh-test PRIVATE Lotus::RenderExtraction)
add_test(NAME lotus-renderer-scene-mesh COMMAND lotus-render-world-mesh-test)

add_executable(lotus-scene-update-test scene-update-test.cpp)
target_link_libraries(lotus-scene-update-test PRIVATE Lotus::RenderExtraction)
add_test(NAME lotus-renderer-scene-update COMMAND lotus-scene-update-test)

add_test(NAME lotus-renderer-host-capability-gate
  COMMAND "${CMAKE_COMMAND}"
    "-DGATE=${CMAKE_SOURCE_DIR}/adapters/hydra2/tests/check_gpu_evidence.cmake"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/host-capability-gate"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/host-capability-gate-test.cmake")

add_test(NAME lotus-renderer-evidence
  COMMAND "$<TARGET_FILE:lotus-headless>"
    --report "${CMAKE_BINARY_DIR}/renderer-ctest-report.json")

add_test(NAME lotus-renderer-install-tree
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DCONFIG=$<CONFIG>"
    "-DPREFIX=${CMAKE_BINARY_DIR}/renderer-install"
    -P "${CMAKE_CURRENT_BINARY_DIR}/run-install-smoke.cmake")
set_tests_properties(lotus-renderer-install-tree PROPERTIES RUN_SERIAL TRUE)
