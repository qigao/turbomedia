add_subdirectory(runtime)
add_subdirectory(codec)
add_subdirectory(containers)
add_subdirectory(speech)
add_subdirectory(recognition)
add_subdirectory(client_processing)
add_subdirectory(player)
add_subdirectory(mobile)
install(
  DIRECTORY speech/include/ recognition/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(
  FILES mobile/turbo_mobile.h
        mobile/turbo_mobile_codec.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

add_subdirectory(tests)
