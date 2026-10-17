# The GPU checks below link the Vulkan loader directly.
if(LOTUS_ENABLE_VULKAN)
  find_package(Vulkan 1.3 REQUIRED)
endif()

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

add_executable(lotus-viewport-camera-test viewport-camera-test.cpp
  ../adapters/viewport/orbit_camera.cpp)
target_link_libraries(lotus-viewport-camera-test PRIVATE Lotus::Warnings)
target_compile_features(lotus-viewport-camera-test PRIVATE cxx_std_20)
add_test(NAME lotus-viewport-camera COMMAND lotus-viewport-camera-test)

# The controls name the backend's settings types; the headers are enough.
add_executable(lotus-viewport-options-test viewport-options-test.cpp
  ../adapters/viewport/options.cpp
  ../adapters/viewport/controls.cpp)
target_link_libraries(lotus-viewport-options-test PRIVATE
  Lotus::RenderExtraction Lotus::Warnings)
target_compile_features(lotus-viewport-options-test PRIVATE cxx_std_20)
add_test(NAME lotus-viewport-options COMMAND lotus-viewport-options-test)

add_executable(lotus-viewport-controls-test viewport-controls-test.cpp
  ../adapters/viewport/controls.cpp)
target_link_libraries(lotus-viewport-controls-test PRIVATE
  Lotus::RenderExtraction Lotus::Warnings)
target_compile_features(lotus-viewport-controls-test PRIVATE cxx_std_20)
add_test(NAME lotus-viewport-controls COMMAND lotus-viewport-controls-test)

add_executable(lotus-viewport-timings-test viewport-timings-test.cpp
  ../adapters/viewport/timings.cpp)
target_link_libraries(lotus-viewport-timings-test PRIVATE
  Lotus::RenderExtraction Lotus::Warnings)
target_compile_features(lotus-viewport-timings-test PRIVATE cxx_std_20)
add_test(NAME lotus-viewport-timings COMMAND lotus-viewport-timings-test)

add_executable(lotus-render-world-mesh-test render-world-mesh-test.cpp)
target_link_libraries(lotus-render-world-mesh-test PRIVATE Lotus::RenderExtraction Lotus::Warnings)
add_test(NAME lotus-renderer-scene-mesh COMMAND lotus-render-world-mesh-test)

add_executable(lotus-scene-update-test scene-update-test.cpp)
target_link_libraries(lotus-scene-update-test PRIVATE Lotus::RenderExtraction Lotus::Warnings)
add_test(NAME lotus-renderer-scene-update COMMAND lotus-scene-update-test)

add_executable(lotus-temporal-test temporal-test.cpp)
target_link_libraries(lotus-temporal-test PRIVATE Lotus::Vulkan Lotus::Warnings)
add_test(NAME lotus-renderer-temporal COMMAND lotus-temporal-test)

add_executable(lotus-reservoir-test reservoir-test.cpp)
target_link_libraries(lotus-reservoir-test PRIVATE Lotus::Vulkan Lotus::Warnings)
add_test(NAME lotus-renderer-reservoir COMMAND lotus-reservoir-test)

add_executable(lotus-restir-statistics-test restir-statistics-test.cpp)
target_link_libraries(lotus-restir-statistics-test PRIVATE Lotus::Warnings)
target_compile_features(lotus-restir-statistics-test PRIVATE cxx_std_20)
add_test(NAME lotus-renderer-restir-statistics COMMAND lotus-restir-statistics-test)

add_executable(lotus-temporal-gpu-test temporal-gpu-test.cpp)
target_link_libraries(lotus-temporal-gpu-test PRIVATE Lotus::Vulkan Lotus::Warnings)
if(LOTUS_ENABLE_VULKAN)
  # The GPU checks load the backend's shaders from its build directory.
  set(_shaders "$<TARGET_PROPERTY:lotus-shaders,LOTUS_SHADER_DIRECTORY>")
  add_dependencies(lotus-temporal-gpu-test lotus-shaders)
  add_test(NAME lotus-renderer-temporal-gpu
    COMMAND lotus-temporal-gpu-test "${_shaders}")
  add_test(NAME lotus-renderer-temporal-gpu-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-temporal-gpu-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/temporal-no-driver"
      "-DARGUMENTS=${_shaders}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
else()
  add_test(NAME lotus-renderer-temporal-gpu COMMAND lotus-temporal-gpu-test)
endif()
set_tests_properties(lotus-renderer-temporal-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(lotus-restir-gpu-test restir-gpu-test.cpp ../adapters/headless/reference.cpp)
target_link_libraries(lotus-restir-gpu-test PRIVATE Lotus::Vulkan Lotus::Warnings)
if(LOTUS_ENABLE_VULKAN)
  add_dependencies(lotus-restir-gpu-test lotus-shaders)
  add_test(NAME lotus-renderer-restir-gpu COMMAND lotus-restir-gpu-test
    "${_shaders}" "${CMAKE_BINARY_DIR}/restir-initial-images")
  add_test(NAME lotus-renderer-restir-gpu-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-restir-gpu-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-no-driver"
      "-DARGUMENTS=${_shaders}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
else()
  add_test(NAME lotus-renderer-restir-gpu COMMAND lotus-restir-gpu-test)
endif()
set_tests_properties(lotus-renderer-restir-gpu PROPERTIES SKIP_RETURN_CODE 77)

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

if(LOTUS_ENABLE_VULKAN)
  add_test(NAME lotus-renderer-benchmark-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-no-driver"
      -DNO_DRIVER=ON
      -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmark-test.cmake")
  add_executable(lotus-acceleration-compaction-test acceleration-compaction-test.cpp)
  target_link_libraries(lotus-acceleration-compaction-test PRIVATE Lotus::Vulkan Vulkan::Vulkan Lotus::Warnings)
  add_test(NAME lotus-renderer-acceleration-compaction COMMAND lotus-acceleration-compaction-test)
  set_tests_properties(lotus-renderer-acceleration-compaction PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME lotus-renderer-acceleration-compaction-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-acceleration-compaction-test>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/compaction-no-driver"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/synchronization-no-driver-test.cmake")
  add_executable(lotus-memory-ranges-test memory-ranges-test.cpp)
  target_link_libraries(lotus-memory-ranges-test PRIVATE Lotus::Vulkan Vulkan::Vulkan Lotus::Warnings)
  add_test(NAME lotus-renderer-memory-ranges COMMAND lotus-memory-ranges-test)
  add_executable(lotus-memory-pool-test memory-pool-test.cpp)
  target_link_libraries(lotus-memory-pool-test PRIVATE Lotus::Vulkan Vulkan::Vulkan Lotus::Warnings)
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
    Vulkan::Vulkan Lotus::RenderExtraction Lotus::Warnings)
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

# ReSTIR checks assert reports and validation messages, never timing, so they
# may share the GPU under `ost test --jobs N`. The fixed performance baseline
# and install tree above stay serial.
foreach(integrator reference wavefront)
  add_test(NAME lotus-renderer-restir-controls-${integrator}
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-controls-${integrator}"
      "-DINTEGRATOR=${integrator}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-controls-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}"
      "-DINTEGRATOR=${integrator}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-mixed
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-mixed"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=mixed-boundaries
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-glossy
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-glossy"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=glossy-disocclusion
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-spheres
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-spheres"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=glossy-spheres
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-multibounce
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-multibounce"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=glossy-spheres
      -DBOUNCES=4 -DREFERENCE_SEED=18874368
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-interreflection
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-interreflection"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=interreflection -DBOUNCES=4
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  add_test(NAME lotus-renderer-restir-benchmark-${integrator}-animation
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-animation"
      "-DINTEGRATOR=${integrator}" -DWORKLOAD=animation
      -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  foreach(proposal uniform power-uniform)
    add_test(NAME lotus-renderer-restir-benchmark-${integrator}-${proposal}
      COMMAND "${CMAKE_COMMAND}"
        "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-${integrator}-${proposal}"
        "-DINTEGRATOR=${integrator}" "-DPROPOSAL=${proposal}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
  endforeach()
endforeach()
if(LOTUS_ENABLE_VULKAN)
  add_test(NAME lotus-renderer-restir-benchmark-no-driver
    COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:lotus-headless>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/restir-benchmark-no-driver"
      -DNO_DRIVER=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/restir-benchmark-test.cmake")
endif()
