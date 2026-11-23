add_subdirectory(transport)
add_subdirectory(rtsp)

if(TURBO_MEDIA_HAS_DESKTOP_SERVICES)
  add_subdirectory(streamer)
  add_subdirectory(pipeline)
  install(
    DIRECTORY streamer/include/ pipeline/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
endif()
