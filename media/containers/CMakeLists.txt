add_subdirectory(common)
add_subdirectory(muxer)
add_subdirectory(demuxer)

install(
  DIRECTORY muxer/include/ demuxer/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")

turbo_media_add_module_test(turbo_media_test_containers tests/test_containers.c
  TurboMedia::Muxer TurboMedia::Demuxer)
