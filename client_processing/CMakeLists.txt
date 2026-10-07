add_library(
  turbo_media_client_processing SHARED
  client_processing.c
  core/client_processing_queues.c
  adapters/client_processing_salts.c
  ffmpeg/client_processing_ffmpeg.c)
turbo_media_configure_component(turbo_media_client_processing ClientProcessing)
target_include_directories(
  turbo_media_client_processing
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  turbo_media_client_processing
  PUBLIC Salts::Capture Salts::Playback
  PRIVATE Salts::Platform
          TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avutil
          TurboMediaFFmpeg::swscale)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
