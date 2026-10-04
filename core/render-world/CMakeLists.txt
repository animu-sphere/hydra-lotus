add_library(lotus-render-world STATIC render_world.cpp)
add_library(Lotus::RenderWorld ALIAS lotus-render-world)
set_target_properties(lotus-render-world PROPERTIES EXPORT_NAME RenderWorld)
target_include_directories(lotus-render-world PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(lotus-render-world PUBLIC cxx_std_20)
