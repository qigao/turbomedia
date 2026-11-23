unset(RE2C_EXECUTABLE CACHE)
if(DEFINED ENV{RE2C_ROOT})
  if(NOT IS_DIRECTORY "$ENV{RE2C_ROOT}")
    message(FATAL_ERROR "RE2C_ROOT must name the restored host re2c package")
  endif()
  find_program(RE2C_EXECUTABLE NAMES re2c
               PATHS "$ENV{RE2C_ROOT}/bin"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
else()
  find_program(RE2C_EXECUTABLE NAMES re2c NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endif()

if(NOT IS_DIRECTORY "$ENV{LEMON_ROOT}")
  message(FATAL_ERROR "LEMON_ROOT must name the restored host Lemon package")
endif()

unset(LEMON_EXECUTABLE CACHE)
unset(LEMPAR CACHE)
find_program(LEMON_EXECUTABLE NAMES lemon
             PATHS "$ENV{LEMON_ROOT}/bin"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_file(LEMPAR NAMES lempar.c
          PATHS "$ENV{LEMON_ROOT}/share/lemon"
          NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

function(turbo_media_find_idlc output_variable)
  if(CMAKE_CROSSCOMPILING)
    set(_turbomedia_idlc_root_name SALTS_UTILS_HOST_ROOT)
  else()
    set(_turbomedia_idlc_root_name SALTS_UTILS_ROOT)
  endif()
  if(NOT DEFINED ENV{${_turbomedia_idlc_root_name}} OR
     NOT IS_DIRECTORY "$ENV{${_turbomedia_idlc_root_name}}")
    message(FATAL_ERROR
      "${_turbomedia_idlc_root_name} must name the host SaltsUtils SDK")
  endif()
  # A previous configure must not retain a compiler from a different SDK.
  unset(${output_variable} CACHE)
  unset(${output_variable})
  find_program(${output_variable} NAMES salts-idlc
               PATHS "$ENV{${_turbomedia_idlc_root_name}}/bin"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  set(${output_variable} "${${output_variable}}" PARENT_SCOPE)
endfunction()
