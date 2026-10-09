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
    --report "${CMAKE_BINARY_DIR}/renderer-ctest-report.json"
    --images "${CMAKE_BINARY_DIR}/reference-images")

add_test(NAME lotus-renderer-install-tree
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DCONFIG=$<CONFIG>"
    "-DPREFIX=${CMAKE_BINARY_DIR}/renderer-install"
    -P "${CMAKE_CURRENT_BINARY_DIR}/run-install-smoke.cmake")
set_tests_properties(lotus-renderer-install-tree PROPERTIES RUN_SERIAL TRUE)

if(LOTUS_WITH_VULKAN)
  add_executable(lotus-memory-ranges-test memory-ranges-test.cpp)
  target_link_libraries(lotus-memory-ranges-test PRIVATE Lotus::Vulkan Vulkan::Vulkan)
  add_test(NAME lotus-renderer-memory-ranges COMMAND lotus-memory-ranges-test)
  add_executable(lotus-memory-pool-test memory-pool-test.cpp)
  target_link_libraries(lotus-memory-pool-test PRIVATE Lotus::Vulkan Vulkan::Vulkan)
  add_test(NAME lotus-renderer-memory-pool COMMAND lotus-memory-pool-test)
  set_tests_properties(lotus-renderer-memory-pool PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME lotus-renderer-memory-pool-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-memory-pool-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/memory-pool-no-driver"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")

  add_executable(lotus-synchronization-validation-test
    synchronization-validation-test.cpp)
  target_link_libraries(lotus-synchronization-validation-test PRIVATE
    Vulkan::Vulkan Lotus::RenderExtraction)
  add_test(NAME lotus-renderer-synchronization-validation
    COMMAND lotus-synchronization-validation-test)
  set_tests_properties(lotus-renderer-synchronization-validation PROPERTIES
    SKIP_RETURN_CODE 77)
  add_test(NAME lotus-renderer-synchronization-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-synchronization-validation-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/synchronization-no-driver"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
endif()
