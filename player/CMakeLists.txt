add_library(turbo_media_player SHARED ffmpeg_player.c)
turbo_media_configure_component(turbo_media_player Player)
target_link_libraries(
  turbo_media_player
  PUBLIC TurboMedia::Codec TurboMedia::Demuxer TurboMedia::Device
  PRIVATE TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avutil
          TurboMediaFFmpeg::swresample
          TurboMediaFFmpeg::swscale)
