if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED TARGET_SYSTEM OR TARGET_SYSTEM STREQUAL "")
  message(FATAL_ERROR "TARGET_SYSTEM is required")
endif()

set(_build_dir "${CMAKE_CURRENT_BINARY_DIR}/product-reject-${TARGET_SYSTEM}")
file(REMOVE_RECURSE "${_build_dir}")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${SOURCE_DIR}"
          -B "${_build_dir}"
          -DTURBO_MEDIA_PRODUCT=SERVER
          -DCMAKE_SYSTEM_NAME=${TARGET_SYSTEM}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)

set(_combined "${_stdout}\n${_stderr}")

if(_result EQUAL 0)
  message(FATAL_ERROR
          "SERVER configure unexpectedly succeeded for ${TARGET_SYSTEM}")
endif()

if(NOT _combined MATCHES
   "TurboMedia SERVER supports only Windows and Linux.*target system: ${TARGET_SYSTEM}")
  message(FATAL_ERROR
          "SERVER ${TARGET_SYSTEM} did not fail at the product gate:\n${_combined}")
endif()

foreach(_unexpected
        "Could NOT find"
        "Salts"
        "vcpkg"
        "Android NDK"
        "CMAKE_C_COMPILER"
        "CMAKE_CXX_COMPILER"
        "Xcode")
  if(_combined MATCHES "${_unexpected}")
    message(FATAL_ERROR
            "SERVER ${TARGET_SYSTEM} reached dependency/toolchain discovery before rejection:\n${_combined}")
  endif()
endforeach()

message(STATUS
        "Verified SERVER rejection for ${TARGET_SYSTEM} before compiler/dependency discovery")
