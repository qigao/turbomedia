add_library(
  turbo_media_muxer STATIC
  muxer_registry.c
  flv_muxer.c
  mp4_muxer.c
  mkv_muxer.c
  mpeg_muxer.c
  flv/flv_muxer_impl.c
  flv/flv_writer.c
  mov/mov-writer.c
  mov/fmp4-writer.c
  ../common/mov/mov-stsd.c
  ../common/mov/mov-tx3g.c
  mkv/mkv-writer.c
)
turbo_media_configure_component(turbo_media_muxer Muxer)
set_target_properties(turbo_media_muxer PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(turbo_media_muxer PRIVATE $<TARGET_OBJECTS:turbo_media_common_objects>)
target_include_directories(
  turbo_media_muxer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE flv
          mov
          mkv
          ${PROJECT_SOURCE_DIR}/common
          ${PROJECT_SOURCE_DIR}/common/codec_helpers
          ${PROJECT_SOURCE_DIR}/common/flv
          ${PROJECT_SOURCE_DIR}/common/mkv
          ${PROJECT_SOURCE_DIR}/common/mov)
target_link_libraries(
  turbo_media_muxer
  PUBLIC TurboMedia::Codec
  PRIVATE TurboUtils::Core
          TurboUtils::STL
          TurboMediaFFmpeg::avformat
          TurboMediaFFmpeg::avcodec
          TurboMediaFFmpeg::avutil)
target_compile_definitions(
  turbo_media_muxer
  PUBLIC TURBO_MEDIA_HAS_FLV TURBO_MEDIA_HAS_MP4 TURBO_MEDIA_HAS_MKV
         TURBO_MEDIA_HAS_MPEG
  PRIVATE MOV_WRITER_ONLY)
