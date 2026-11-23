add_library(turbo_media_pipeline SHARED
  src/turbo_pipeline.c
  src/turbo_pipeline_compositor.c)

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
          TurboMediaFFmpeg::swscale
          TurboMediaFFmpeg::avutil)
if(UNIX)
  target_link_libraries(turbo_media_pipeline PRIVATE m)
endif()
if(WIN32)
  target_sources(turbo_media_pipeline PRIVATE src/pipeline_compositor_gpu.c)
  target_link_libraries(turbo_media_pipeline PRIVATE d3d11 d3dcompiler dxgi dxguid)
endif()

turbo_media_add_module_test(turbo_media_test_pipeline tests/test_pipeline.c
  TurboMedia::Pipeline TurboMedia::MediaRuntime TurboMedia::RTSP TurboMedia::ServerRTSP
  TurboMedia::RTP TurboMedia::Codec CHttp::Client CHttp::App Salts::Core)
turbo_media_add_module_test(turbo_media_test_pipeline_compositor tests/test_compositor.c
  TurboMedia::Pipeline TurboMediaFFmpeg::avformat TurboMediaFFmpeg::avcodec)
if(WIN32)
  target_link_libraries(turbo_media_test_pipeline PRIVATE ws2_32)
endif()
