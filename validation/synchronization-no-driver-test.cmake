# Exercise the hosted runner's missing-driver case even on GPU workstations.
if(NOT DEFINED EXECUTABLE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "EXECUTABLE and TEST_ROOT are required")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(manifest "${TEST_ROOT}/unavailable-manifest.json")
if(EXISTS "${manifest}")
  message(FATAL_ERROR "The missing-driver manifest must not exist: ${manifest}")
endif()
# Limit discovery to a missing manifest in this child process; the workstation
# drivers and parent CTest process are unaffected. Execute directly to retain
# the executable's exact exit code rather than cmake -E env's failure status.
set(ENV{VK_DRIVER_FILES} "${manifest}")
set(ENV{VK_ICD_FILENAMES} "${manifest}")
execute_process(
  COMMAND "${EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 30)
if(NOT result STREQUAL "77" OR
   NOT output MATCHES "SKIP:.*VK_ERROR_INCOMPATIBLE_DRIVER")
  message(FATAL_ERROR
    "Missing Vulkan driver must produce an explained SKIP (77), got ${result}:\n${output}\n${error}")
endif()
message(STATUS "Missing Vulkan driver produces an explained SKIP (77)")
