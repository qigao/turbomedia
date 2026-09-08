add_library(turbo_media_recognition SHARED turbo_recognition.c)
turbo_media_configure_component(turbo_media_recognition Recognition)
target_include_directories(
  turbo_media_recognition
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_recognition PRIVATE Salts::Core)
if(TURBO_MEDIA_IS_CLIENT)
  target_link_libraries(turbo_media_recognition PUBLIC TurboMedia::Player)
endif()

cmake_add_test(
  turbo_media_test_recognition
  SOURCES tests/test_recognition.c
  LIBS TurboMedia::Recognition Salts::TinyTest
  FOLDER "turbomedia/tests/recognition")
