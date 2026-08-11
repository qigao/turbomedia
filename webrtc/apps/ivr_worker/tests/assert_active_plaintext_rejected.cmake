if(NOT DEFINED PROGRAM OR NOT DEFINED CONTENT_ROOT)
  message(FATAL_ERROR "PROGRAM and CONTENT_ROOT are required")
endif()

execute_process(
  COMMAND "${PROGRAM}" --content-root "${CONTENT_ROOT}"
          --router-port 17731 --pub-port 17732
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)

if(result EQUAL 0)
  message(FATAL_ERROR "active plaintext worker unexpectedly succeeded")
endif()

set(output "${stdout}\n${stderr}")
string(FIND "${output}" "active mode requires FlowMQ mTLS" match_offset)
if(match_offset EQUAL -1)
  message(FATAL_ERROR
          "active plaintext worker failed without the required mTLS error: ${output}")
endif()
