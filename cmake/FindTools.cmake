find_program(RE2C_EXECUTABLE NAMES re2c REQUIRED)

if(NOT TARGET lemon)
  message(FATAL_ERROR "The required in-tree lemon target is missing")
endif()

set(LEMPAR "${PROJECT_SOURCE_DIR}/tools/lemon/lempar.c")
if(NOT EXISTS "${LEMPAR}")
  message(FATAL_ERROR "The required lemon template is missing: ${LEMPAR}")
endif()
