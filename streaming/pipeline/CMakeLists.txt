add_library(turbo_media_pipeline SHARED src/turbo_pipeline.c)

turbo_media_configure_component(turbo_media_pipeline Pipeline)

target_include_directories(
  turbo_media_pipeline
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(
  turbo_media_pipeline
  PUBLIC Salts::Core
  PRIVATE Salts::DataBind
          Salts::CYaml
          TurboMedia::MediaRuntime
          TurboMedia::RTP
          TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avfilter
          TurboMediaFFmpeg::avutil)

turbo_media_add_module_test(turbo_media_test_pipeline tests/test_pipeline.c
  TurboMedia::Pipeline TurboMedia::MediaRuntime TurboMedia::RTSP TurboMedia::ServerRTSP
  TurboMedia::RTP TurboMedia::Codec CHttp::Client CHttp::App Salts::Core)
if(WIN32)
  target_link_libraries(turbo_media_test_pipeline PRIVATE ws2_32)
endif()
