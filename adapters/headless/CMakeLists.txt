include(LotusShaders)

add_executable(lotus-headless main.cpp reference.cpp workloads.cpp benchmark.cpp restir_benchmark.cpp
  svgf_benchmark.cpp)
target_link_libraries(lotus-headless PRIVATE
  Lotus::RenderWorld
  Lotus::RenderExtraction
  Lotus::Vulkan
  Lotus::Warnings)
target_compile_features(lotus-headless PRIVATE cxx_std_20)
target_compile_definitions(lotus-headless PRIVATE
  LOTUS_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  LOTUS_BENCHMARK_CONFIG="$<CONFIG>"
  LOTUS_BENCHMARK_SYSTEM="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
# The report explains which Hydra assertions its OpenUSD CTests own.
if(LOTUS_ENABLE_HYDRA2)
  target_compile_definitions(lotus-headless PRIVATE LOTUS_HAS_HYDRA2=1)
endif()

# The committed reference images, beside the executable as the shaders are.
set(_reference_files
  "${PROJECT_SOURCE_DIR}/validation/reference/cornell-box-mean.pfm"
  "${PROJECT_SOURCE_DIR}/validation/reference/cornell-box-variance.pfm")
add_custom_command(TARGET lotus-headless POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:lotus-headless>/reference"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_reference_files}
    "$<TARGET_FILE_DIR:lotus-headless>/reference"
  VERBATIM)
set_property(TARGET lotus-headless APPEND PROPERTY LINK_DEPENDS
  ${_reference_files})
# The third-party assets the checks read, with their licence and manifest.
set(_asset_files
  "${PROJECT_SOURCE_DIR}/validation/assets/StinsonBeach.hdr"
  "${PROJECT_SOURCE_DIR}/validation/assets/LICENSE.OpenUSD.txt"
  "${PROJECT_SOURCE_DIR}/validation/assets/manifest.json")
add_custom_command(TARGET lotus-headless POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:lotus-headless>/assets"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_asset_files}
    "$<TARGET_FILE_DIR:lotus-headless>/assets"
  VERBATIM)
set_property(TARGET lotus-headless APPEND PROPERTY LINK_DEPENDS
  ${_asset_files})

if(LOTUS_ENABLE_VULKAN)
  lotus_stage_shaders(TARGET lotus-headless)
endif()

# After the shader copy above, so the report renders with this build's shaders.
if(NOT CMAKE_CROSSCOMPILING)
  add_custom_command(TARGET lotus-headless POST_BUILD
    COMMAND "$<TARGET_FILE:lotus-headless>"
      --report "${CMAKE_BINARY_DIR}/renderer-report.json"
      --images "${CMAKE_BINARY_DIR}/reference-images"
    COMMENT "Writing renderer-report.json"
    VERBATIM)
endif()

# The installed tree mirrors the build tree; the backend installs bin/shaders.
install(TARGETS lotus-headless
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ${_reference_files}
  DESTINATION "${CMAKE_INSTALL_BINDIR}/reference")
install(FILES ${_asset_files}
  DESTINATION "${CMAKE_INSTALL_BINDIR}/assets")
