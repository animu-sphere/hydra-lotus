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

add_executable(lotus-temporal-test temporal-test.cpp)
target_link_libraries(lotus-temporal-test PRIVATE Lotus::Vulkan)
add_test(NAME lotus-renderer-temporal COMMAND lotus-temporal-test)

add_executable(lotus-reservoir-test reservoir-test.cpp)
target_link_libraries(lotus-reservoir-test PRIVATE Lotus::Vulkan)
add_test(NAME lotus-renderer-reservoir COMMAND lotus-reservoir-test)

add_executable(lotus-restir-statistics-test restir-statistics-test.cpp)
target_compile_features(lotus-restir-statistics-test PRIVATE cxx_std_20)
add_test(NAME lotus-renderer-restir-statistics COMMAND lotus-restir-statistics-test)

add_executable(lotus-temporal-gpu-test temporal-gpu-test.cpp)
target_link_libraries(lotus-temporal-gpu-test PRIVATE Lotus::Vulkan)
if(LOTUS_WITH_VULKAN)
  add_dependencies(lotus-temporal-gpu-test lotus-renderer-shaders)
  add_test(NAME lotus-renderer-temporal-gpu COMMAND lotus-temporal-gpu-test
    "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}"
    "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}" "${LOTUS_WAVEFRONT_SPV}")
  add_test(NAME lotus-renderer-temporal-gpu-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-temporal-gpu-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/temporal-no-driver"
      "-DARGUMENTS=${LOTUS_VERTEX_SPV};${LOTUS_FRAGMENT_SPV};${LOTUS_RAY_VERTEX_SPV};${LOTUS_RAY_FRAGMENT_SPV};${LOTUS_WAVEFRONT_SPV}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
else()
  add_test(NAME lotus-renderer-temporal-gpu COMMAND lotus-temporal-gpu-test)
endif()
set_tests_properties(lotus-renderer-temporal-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(lotus-restir-gpu-test restir-gpu-test.cpp ../adapters/headless/reference.cpp)
target_link_libraries(lotus-restir-gpu-test PRIVATE Lotus::Vulkan)
if(LOTUS_WITH_VULKAN)
  add_dependencies(lotus-restir-gpu-test lotus-renderer-shaders)
  add_test(NAME lotus-renderer-restir-gpu COMMAND lotus-restir-gpu-test
    "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}"
    "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}" "${LOTUS_WAVEFRONT_SPV}"
    "${CMAKE_BINARY_DIR}/restir-initial-images")
  add_test(NAME lotus-renderer-restir-gpu-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-restir-gpu-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-no-driver"
      "-DARGUMENTS=${LOTUS_VERTEX_SPV};${LOTUS_FRAGMENT_SPV};${LOTUS_RAY_VERTEX_SPV};${LOTUS_RAY_FRAGMENT_SPV};${LOTUS_WAVEFRONT_SPV}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
else()
  add_test(NAME lotus-renderer-restir-gpu COMMAND lotus-restir-gpu-test)
endif()
set_tests_properties(lotus-renderer-restir-gpu PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# The committed third-party validation assets match their manifest.
add_test(NAME lotus-validation-assets
  COMMAND "${CMAKE_COMMAND}"
    "-DASSETS=${CMAKE_CURRENT_SOURCE_DIR}/assets"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assets-test.cmake")

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

add_test(NAME lotus-renderer-benchmark
  COMMAND "${CMAKE_COMMAND}"
    "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmark-test.cmake")
set_tests_properties(lotus-renderer-benchmark PROPERTIES RUN_SERIAL TRUE)
add_test(NAME lotus-renderer-benchmark-wavefront
  COMMAND "${CMAKE_COMMAND}"
    "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-wavefront"
    -DINTEGRATOR=wavefront
    -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmark-test.cmake")
set_tests_properties(lotus-renderer-benchmark-wavefront PROPERTIES RUN_SERIAL TRUE)
add_test(NAME lotus-renderer-benchmark-mis
  COMMAND "${CMAKE_COMMAND}"
    "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-mis"
    -DLIGHT_SAMPLING=mis
    -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmark-test.cmake")
set_tests_properties(lotus-renderer-benchmark-mis PROPERTIES RUN_SERIAL TRUE)

if(LOTUS_WITH_VULKAN)
  add_test(NAME lotus-renderer-benchmark-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-no-driver"
      -DNO_DRIVER=ON
      -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmark-test.cmake")
  add_executable(lotus-acceleration-compaction-test acceleration-compaction-test.cpp)
  target_link_libraries(lotus-acceleration-compaction-test PRIVATE Lotus::Vulkan Vulkan::Vulkan)
  add_test(NAME lotus-renderer-acceleration-compaction COMMAND lotus-acceleration-compaction-test)
  set_tests_properties(lotus-renderer-acceleration-compaction PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME lotus-renderer-acceleration-compaction-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-acceleration-compaction-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/compaction-no-driver"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
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

foreach(integrator reference wavefront)
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}"
      "-DINTEGRATOR=${integrator}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  set_tests_properties(lotus-renderer-restir-benchmark-${integrator} PROPERTIES RUN_SERIAL TRUE)
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-mixed
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-mixed"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=mixed-boundaries
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  set_tests_properties(lotus-renderer-restir-benchmark-${integrator}-mixed PROPERTIES RUN_SERIAL TRUE)
  foreach(proposal uniform power-uniform)
    add_test(NAME lotus-renderer-restir-benchmark-${integrator}-${proposal}
      COMMAND "${CMAKE_COMMAND}"
        "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-${proposal}"
        "-DINTEGRATOR=${integrator}" "-DPROPOSAL=${proposal}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
    set_tests_properties(lotus-renderer-restir-benchmark-${integrator}-${proposal} PROPERTIES RUN_SERIAL TRUE)
  endforeach()
endforeach()
if(LOTUS_WITH_VULKAN)
  add_test(NAME lotus-renderer-restir-benchmark-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-no-driver"
      -DNO_DRIVER=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
endif()
