add_library(turbo_media_pipeline SHARED src/turbo_pipeline.c)

turbo_media_configure_component(turbo_media_pipeline Pipeline)

target_include_directories(
  turbo_media_pipeline
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(
  turbo_media_pipeline
  PUBLIC TurboUtils::Core
  PRIVATE TurboParser::DataBind
          TurboParser::Parser
          TurboMedia::Server
          TurboMedia::Transport
          TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avfilter
          TurboMediaFFmpeg::avutil)
