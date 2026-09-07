if(TURBO_MEDIA_IS_CLIENT)
  add_executable(capture_save_image capture_save_image.c)
  target_link_libraries(capture_save_image PRIVATE Salts::Capture)
  set_target_properties(capture_save_image PROPERTIES FOLDER "turbomedia/examples")
endif()

add_executable(codec_g711_smoke codec_g711_smoke.c)
target_link_libraries(codec_g711_smoke PRIVATE TurboMedia::Codec)
set_target_properties(codec_g711_smoke PROPERTIES FOLDER "turbomedia/examples")

if(TURBO_MEDIA_IS_CLIENT)
  add_executable(ffmpeg_probe ffmpeg_probe.c)
  target_link_libraries(ffmpeg_probe PRIVATE TurboMedia::Player)
  set_target_properties(ffmpeg_probe PROPERTIES FOLDER "turbomedia/examples")
endif()

if(TURBO_MEDIA_IS_SERVER)
  add_executable(turbo_pipeline_run pipeline_run.c)
  target_link_libraries(
    turbo_pipeline_run PRIVATE TurboMedia::Pipeline Salts::Core)
  set_target_properties(turbo_pipeline_run PROPERTIES FOLDER "turbomedia/examples")
endif()
