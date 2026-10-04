add_library(lotus-render-extraction STATIC extraction.cpp)
add_library(Lotus::RenderExtraction ALIAS lotus-render-extraction)
set_target_properties(lotus-render-extraction PROPERTIES EXPORT_NAME RenderExtraction)
target_include_directories(lotus-render-extraction PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(lotus-render-extraction PUBLIC Lotus::RenderWorld)
target_compile_features(lotus-render-extraction PUBLIC cxx_std_20)
