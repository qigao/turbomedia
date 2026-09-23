# Product/profile validation intentionally runs before project() when the target
# system is already known from a preset or -D CMAKE_SYSTEM_NAME. This makes
# unsupported SERVER targets fail before compiler, SDK, vcpkg, or package
# discovery can mask the product boundary. It is run again after project() to
# cover toolchain files that establish CMAKE_SYSTEM_NAME during language setup.

set(TURBO_MEDIA_PRODUCT "" CACHE STRING "TurboMedia product: CLIENT or SERVER")
set_property(CACHE TURBO_MEDIA_PRODUCT PROPERTY STRINGS CLIENT SERVER)

function(turbo_media_validate_product_target)
  if(NOT TURBO_MEDIA_PRODUCT STREQUAL "CLIENT" AND
     NOT TURBO_MEDIA_PRODUCT STREQUAL "SERVER")
    message(FATAL_ERROR "TURBO_MEDIA_PRODUCT must be CLIENT or SERVER")
  endif()

  if(DEFINED CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "")
    set(_turbo_media_target_system "${CMAKE_SYSTEM_NAME}")
  else()
    set(_turbo_media_target_system "${CMAKE_HOST_SYSTEM_NAME}")
  endif()

  if(TURBO_MEDIA_PRODUCT STREQUAL "SERVER" AND
     NOT _turbo_media_target_system STREQUAL "Windows" AND
     NOT _turbo_media_target_system STREQUAL "Linux")
    message(FATAL_ERROR
      "TurboMedia SERVER supports only Windows and Linux "
      "(target system: ${_turbo_media_target_system})")
  endif()
endfunction()
