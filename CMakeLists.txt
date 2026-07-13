cmake_minimum_required(VERSION 3.20)

project(
  TurboMedia
  VERSION 1.0.0
  LANGUAGES C)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)
add_subdirectory(tools)
include(FindTools)

include(cmake/TurboMediaDependencies.cmake)
include(cmake/TurboMediaComponentTargets.cmake)

if(BUILD_TESTS)
  enable_testing()
endif()

if(TURBO_MEDIA_ENABLE_WEBRTC)
  add_subdirectory(webrtc)
endif()

if(TURBO_MEDIA_ENABLE_RTSP)
  include(media/rtsp/CMakeLists.txt)
  if(TARGET turbo_media_server AND TARGET turbo_media_rtsp)
    target_sources(turbo_media_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server/rtsp_server_adapter.c)
    target_link_libraries(turbo_media_server PUBLIC turbo_media_rtsp)
  endif()
endif()

if(BUILD_MOBILE)
  add_subdirectory(media/mobile)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

include(cmake/TurboMediaInstall.cmake)
