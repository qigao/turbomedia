add_library(turbo_media_player SHARED ffmpeg_player.c)
turbo_media_configure_component(turbo_media_player Player)
if(WIN32)
  set_target_properties(turbo_media_player PROPERTIES OUTPUT_NAME turbo_media_player-2)
endif()
target_link_libraries(
  turbo_media_player
  PUBLIC TurboMedia::Codec TurboMedia::Demuxer Salts::Media
  PRIVATE TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avutil
          TurboMediaFFmpeg::swresample
          TurboMediaFFmpeg::swscale)

turbo_media_add_module_test(turbo_media_test_player_audio tests/test_player_audio.c
  TurboMedia::Player)
turbo_media_add_module_test(turbo_media_test_hls_public tests/test_hls_public.c
  TurboMedia::Player)
