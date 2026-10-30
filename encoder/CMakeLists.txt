# Single C11 public Encoder API. PNG/JPEG have one built-in stb provider;
# WebP requires its exact FFmpeg encoder. No per-format runtime fallback.
# Both dependencies are already required by the parent TurboMedia project.
add_library(turbo_media_encoder SHARED
  src/turbo_media_encoder_image.c
  src/turbo_media_encoder_video.c)
turbo_media_configure_component(turbo_media_encoder Encoder)
target_include_directories(turbo_media_encoder
  PRIVATE ${STB_INCLUDE_DIR}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_encoder PRIVATE
  TurboMedia::Codec TurboMedia::Muxer
  TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avutil TurboMediaFFmpeg::swscale)
set_target_properties(turbo_media_encoder PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
