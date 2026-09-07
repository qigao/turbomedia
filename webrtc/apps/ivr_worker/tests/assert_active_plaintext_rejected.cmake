if(NOT DEFINED PROGRAM)
  message(FATAL_ERROR "PROGRAM is required")
endif()

execute_process(
  COMMAND "${PROGRAM}" --router-port 17731
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)

if(result EQUAL 0)
  message(FATAL_ERROR "active plaintext worker unexpectedly succeeded")
endif()

set(output "${stdout}\n${stderr}")
string(FIND "${output}" "active mode requires CHTTP H1 WebSocket mTLS" match_offset)
if(match_offset EQUAL -1)
  message(FATAL_ERROR
          "active plaintext worker failed without the required mTLS error: ${output}")
endif()
