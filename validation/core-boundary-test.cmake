# Exercise the actual checker against headers added after configuration.
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/core-boundary-fixture")
file(MAKE_DIRECTORY "${fixture}/nested")
file(WRITE "${fixture}/scene.hpp" "#include <array>\n")

function(check_boundary expected)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPUBLIC_HEADER_ROOT=${fixture}" -P "${CHECKER}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(expected STREQUAL "pass")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "valid headers rejected: ${output}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT "${error}" MATCHES "core boundary violation")
    message(FATAL_ERROR "new forbidden header escaped the check: ${output}${error}")
  endif()
endfunction()

# Reset the one fixture file from a previous invocation, without deleting it.
file(WRITE "${fixture}/nested/new.inl" "#include <cstdint>\n")
check_boundary(pass)
foreach(token IN ITEMS "pxr/imaging/hd/renderDelegate.h" "vulkan/vulkan.h"
    "GLFW/glfw3.h" "slang.h")
  file(WRITE "${fixture}/nested/new.inl" "#include <${token}>\n")
  check_boundary(fail)
endforeach()
file(WRITE "${fixture}/nested/new.inl" "#include <cstdint>\n")
check_boundary(pass)
