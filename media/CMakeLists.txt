add_library(
  turbo_media_device SHARED
  playback/miniaudio_impl.c
  playback/turbo_playback.c)
turbo_media_configure_component(turbo_media_device Device)
target_include_directories(
  turbo_media_device
  PRIVATE ${MINIAUDIO_INCLUDE_DIR}
          ${STB_INCLUDE_DIR})
target_link_libraries(turbo_media_device PUBLIC Salts::Capture)
