add_executable(lotus-headless main.cpp reference.cpp benchmark.cpp)
target_link_libraries(lotus-headless PRIVATE
  Lotus::RenderWorld
  Lotus::RenderExtraction
  Lotus::Vulkan)
target_compile_features(lotus-headless PRIVATE cxx_std_20)
target_compile_definitions(lotus-headless PRIVATE
  LOTUS_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  LOTUS_BENCHMARK_CONFIG="$<CONFIG>"
  LOTUS_BENCHMARK_SYSTEM="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")

# The committed reference images, beside the executable as the shaders are.
set(LOTUS_REFERENCE_FILES
  "${CMAKE_SOURCE_DIR}/validation/reference/cornell-box-mean.pfm"
  "${CMAKE_SOURCE_DIR}/validation/reference/cornell-box-variance.pfm")
set(LOTUS_REFERENCE_FILES "${LOTUS_REFERENCE_FILES}" PARENT_SCOPE)
add_custom_command(TARGET lotus-headless POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:lotus-headless>/reference"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LOTUS_REFERENCE_FILES}
    "$<TARGET_FILE_DIR:lotus-headless>/reference"
  VERBATIM)
set_property(TARGET lotus-headless APPEND PROPERTY LINK_DEPENDS
  ${LOTUS_REFERENCE_FILES})

if(LOTUS_WITH_VULKAN)
  add_dependencies(lotus-headless lotus-renderer-shaders)
  # A shader change relinks, so the copies below and the report follow it.
  set_property(TARGET lotus-headless APPEND PROPERTY LINK_DEPENDS
    "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
    "${LOTUS_WAVEFRONT_SPV}" "${LOTUS_VERTEX_SPV}" "${LOTUS_FRAGMENT_SPV}")
  add_custom_command(TARGET lotus-headless POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:lotus-headless>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${LOTUS_RAY_VERTEX_SPV}" "${LOTUS_RAY_FRAGMENT_SPV}"
      "${LOTUS_WAVEFRONT_SPV}" "$<TARGET_FILE_DIR:lotus-headless>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${LOTUS_VERTEX_SPV}"
      "$<TARGET_FILE_DIR:lotus-headless>/shaders/triangle.vert.spv"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${LOTUS_FRAGMENT_SPV}"
      "$<TARGET_FILE_DIR:lotus-headless>/shaders/triangle.frag.spv"
    VERBATIM)
endif()

if(NOT CMAKE_CROSSCOMPILING)
  add_custom_command(TARGET lotus-headless POST_BUILD
    COMMAND "$<TARGET_FILE:lotus-headless>"
      --report "${CMAKE_BINARY_DIR}/renderer-report.json"
      --images "${CMAKE_BINARY_DIR}/reference-images"
    COMMENT "Writing renderer-report.json"
    VERBATIM)
endif()
