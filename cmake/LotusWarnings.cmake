# SPDX-License-Identifier: Apache-2.0
# The compiler policy of Lotus's own targets. Each links it PRIVATE, so it
# never reaches fetched dependencies (GLFW) or package consumers; static
# libraries wrap it in BUILD_INTERFACE to keep it out of the export set.
include_guard(GLOBAL)

add_library(lotus-warnings INTERFACE)
add_library(Lotus::Warnings ALIAS lotus-warnings)
if(MSVC)
  target_compile_options(lotus-warnings INTERFACE
    /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(lotus-warnings INTERFACE
    -Wall -Wextra -Wpedantic)
endif()
