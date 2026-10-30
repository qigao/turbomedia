# One C11 owner of raw-frame image encoding. FFmpeg is already required
# by the parent TurboMedia project, no alternative encoder is selected.
add_library(turbo_media_encoder SHARED src/turbo_media_encoder_image.c)
turbo_media_configure_component(turbo_media_encoder Encoder)
target_include_directories(turbo_media_encoder
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_encoder PRIVATE
  TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avutil TurboMediaFFmpeg::swscale)
set_target_properties(turbo_media_encoder PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
