add_library(turbo_media_speech SHARED turbo_speech.c)
turbo_media_configure_component(turbo_media_speech Speech)
target_include_directories(
  turbo_media_speech
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

cmake_add_test(
  turbo_media_test_speech
  SOURCES tests/test_speech.c
  LIBS TurboMedia::Speech TurboUtils::TinyTest
  FOLDER "turbomedia/tests/speech")
