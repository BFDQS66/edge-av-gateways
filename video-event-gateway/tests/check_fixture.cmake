if(NOT DEFINED GENERATE OR NOT DEFINED DETECT OR NOT DEFINED TEST_DIR)
  message(FATAL_ERROR "GENERATE, DETECT and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
set(video "${TEST_DIR}/fixture.avi")
set(output "${TEST_DIR}/events")

execute_process(COMMAND "${GENERATE}" "${video}"
                RESULT_VARIABLE generate_result
                OUTPUT_VARIABLE generate_output
                ERROR_VARIABLE generate_error)
if(NOT generate_result EQUAL 0)
  message(FATAL_ERROR "fixture generation failed: ${generate_error}")
endif()

execute_process(COMMAND "${DETECT}" --source "${video}" --output "${output}"
                RESULT_VARIABLE detect_result
                OUTPUT_VARIABLE detect_output
                ERROR_VARIABLE detect_error)
if(NOT detect_result EQUAL 0)
  message(FATAL_ERROR "detection failed: ${detect_error}")
endif()

file(STRINGS "${output}/events.jsonl" events)
set(lines 0)
set(zones 0)
foreach(event IN LISTS events)
  string(JSON type GET "${event}" type)
  string(JSON snapshot GET "${event}" snapshot)
  if(NOT EXISTS "${output}/${snapshot}")
    message(FATAL_ERROR "missing event snapshot: ${snapshot}")
  endif()
  if(type STREQUAL "line_cross")
    math(EXPR lines "${lines} + 1")
  elseif(type STREQUAL "zone_enter")
    math(EXPR zones "${zones} + 1")
  endif()
endforeach()
if(NOT lines EQUAL 1 OR NOT zones EQUAL 1)
  message(FATAL_ERROR "expected one event of each type, got line=${lines}, zone=${zones}; ${detect_output}")
endif()
message(STATUS "synthetic video passed: ${detect_output}")
