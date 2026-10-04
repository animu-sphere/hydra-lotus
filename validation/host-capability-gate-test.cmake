if(DEFINED CASE_REPORT)
  include("${GATE}")
  lotus_check_gpu_evidence("${CASE_REPORT}" skip_detail)
  if(NOT skip_detail STREQUAL EXPECTED_SKIP)
    message(FATAL_ERROR "Unexpected skip detail: '${skip_detail}'")
  endif()
  return()
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}")
foreach(case IN ITEMS pass skip fail unknown missing-assertion missing-report unexplained invalid)
  set(report "${TEST_ROOT}/${case}.json")
  set(expected_skip "")
  if(case STREQUAL "missing-report")
    # This filename is never written by this test.
  elseif(case STREQUAL "missing-assertion")
    file(WRITE "${report}" "{\"checks\":[]}")
  elseif(case STREQUAL "invalid")
    file(WRITE "${report}" "not-json")
  elseif(case STREQUAL "unexplained")
    file(WRITE "${report}" "{\"checks\":[{\"id\":\"renderer.gpu.frame\",\"status\":\"skip\",\"detail\":\"\"}]}")
  else()
    if(case STREQUAL "skip")
      set(expected_skip "no Vulkan device")
    endif()
    file(WRITE "${report}" "{\"checks\":[{\"id\":\"renderer.gpu.frame\",\"status\":\"${case}\",\"detail\":\"${expected_skip}\"}]}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DGATE=${GATE}" "-DCASE_REPORT=${report}"
      "-DEXPECTED_SKIP=${expected_skip}" -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(case STREQUAL "pass" OR case STREQUAL "skip")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "${case} was rejected: ${output}${error}")
    endif()
  elseif(result EQUAL 0)
    message(FATAL_ERROR "${case} was accepted")
  endif()
endforeach()

# Exercise the actual host wrapper: capability absence must return before
# deleting a previous staging tree or attempting to launch testusdview.
set(fixture "${TEST_ROOT}/skip-fixture")
file(MAKE_DIRECTORY "${fixture}/stage")
file(WRITE "${fixture}/stage/preserved.txt" "preserve")
file(COPY_FILE "${TEST_ROOT}/skip.json" "${fixture}/renderer-report.json")
get_filename_component(gate_dir "${GATE}" DIRECTORY)
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DRENDERER_BUILD_DIR=${fixture}" "-DRENDERER_CONFIG=Release"
    "-DRENDERER_STAGE_DIR=${fixture}/stage"
    "-DRENDERER_INSTALL_LIBDIR=lib" "-DRENDERER_INSTALL_DATADIR=share"
    "-DRENDERER_PXR_ROOT=unused" "-DRENDERER_PYTHON=unused"
    "-DRENDERER_TESTUSDVIEW=unused" "-DRENDERER_TEST_SCRIPT=unused"
    -P "${gate_dir}/run_usdview_smoke.cmake"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "SKIP usdview smoke: no Vulkan device"
    OR NOT EXISTS "${fixture}/stage/preserved.txt")
  message(FATAL_ERROR "Host wrapper did not skip safely: ${output}${error}")
endif()
