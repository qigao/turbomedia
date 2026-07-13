add_executable(capture_save_image capture_save_image.c)
target_link_libraries(capture_save_image PRIVATE TurboMedia::Device yuv)
target_include_directories(capture_save_image PRIVATE ${Stb_INCLUDE_DIR})
set_target_properties(capture_save_image PROPERTIES FOLDER "turbomedia/examples")

add_executable(codec_g711_smoke codec_g711_smoke.c)
target_link_libraries(codec_g711_smoke PRIVATE TurboMedia::Codec)
set_target_properties(codec_g711_smoke PROPERTIES FOLDER "turbomedia/examples")

if(TURBO_MEDIA_ENABLE_FFMPEG)
  add_executable(ffmpeg_probe ffmpeg_probe.c)
  target_link_libraries(ffmpeg_probe PRIVATE TurboMedia::Player)
  set_target_properties(ffmpeg_probe PROPERTIES FOLDER "turbomedia/examples")
endif()
