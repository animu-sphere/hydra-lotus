add_executable(lotus-headless main.cpp)
target_link_libraries(lotus-headless PRIVATE
  Lotus::RenderWorld
  Lotus::RenderExtraction
  Lotus::Vulkan)
target_compile_features(lotus-headless PRIVATE cxx_std_20)

if(LOTUS_WITH_VULKAN)
  add_dependencies(lotus-headless lotus-renderer-shaders)
  add_custom_command(TARGET lotus-headless POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:lotus-headless>/shaders"
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
    COMMENT "Writing renderer-report.json"
    VERBATIM)
endif()
