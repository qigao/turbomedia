add_library(
  turbo_media_demuxer SHARED
  demuxer_registry.c
  flv_demuxer.c
  mp4_demuxer.c
  mkv_demuxer.c
  mpeg_demuxer.c
  flv/flv_demuxer_impl.c
  flv/flv_demuxer_script.c
  flv/flv_reader.c
  flv/flv_parser.c
  mov/mov-reader.c
  mov/fmp4-reader.c
  ../common/mov/mov-stsd.c
  ../common/mov/mov-tx3g.c
  mkv/mkv-reader.c)
turbo_media_configure_component(turbo_media_demuxer Demuxer)
target_include_directories(
  turbo_media_demuxer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE flv mov mkv ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(
  turbo_media_demuxer
  PUBLIC TurboMedia::Codec TurboMedia::Muxer
  PRIVATE turbo_media_common_objects
          TurboUtils::Core
          TurboUtils::STL
          TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avutil)
target_compile_definitions(
  turbo_media_demuxer
  PUBLIC TURBO_MEDIA_HAS_FLV TURBO_MEDIA_HAS_MP4 TURBO_MEDIA_HAS_MKV
         TURBO_MEDIA_HAS_MPEG
  PRIVATE MOV_READER_ONLY)
