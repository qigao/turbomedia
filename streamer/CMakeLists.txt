set(TURBO_MEDIA_HLS_SOURCES hls/hls-fmp4.c hls/hls-m3u8.c)
set(TURBO_MEDIA_DASH_SOURCES
    dash/dash-adaptation.c
    dash/dash-mpd.c
    dash/dash-representation.c
    dash/dash-segment.c
    dash/xs-duration.c)
file(GLOB TURBO_MEDIA_RTMP_SOURCES CONFIGURE_DEPENDS rtmp/*.c)

add_library(
  turbo_media_streamer SHARED
  streamer_registry.c
  chttp_upload.c
  hls_streamer.c
  dash_streamer.c
  http_flv_streamer.c
  rtmp_streamer.c
  ${TURBO_MEDIA_HLS_SOURCES}
  ${TURBO_MEDIA_DASH_SOURCES}
  ${TURBO_MEDIA_RTMP_SOURCES})
turbo_media_configure_component(turbo_media_streamer Streamer)
target_include_directories(
  turbo_media_streamer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE hls
          dash
          rtmp
          rtmp/include
          ${PROJECT_SOURCE_DIR}/common/codec_helpers
          ${PROJECT_SOURCE_DIR}/common/mov
          ${PROJECT_SOURCE_DIR}/muxer/flv)
target_link_libraries(
  turbo_media_streamer
  PUBLIC TurboMedia::Server
         TurboMedia::Codec
         TurboMedia::Muxer
         TurboMedia::Demuxer
         TurboMedia::Transport
  PRIVATE Salts::Core Salts::CSTL CHttp::Client Salts::CNet)
target_compile_definitions(
  turbo_media_streamer
  PUBLIC TURBO_MEDIA_HAS_HLS TURBO_MEDIA_HAS_DASH TURBO_MEDIA_HAS_RTMP
         TURBO_MEDIA_HAS_HTTP_FLV)

# Internal fault injection is opt-in for the native regression build only.
# No production SDK or public interface contains this error-injection seam.
option(TURBO_MEDIA_TEST_FAULT_INJECTION "Enable HTTP-FLV one-shot owner drain fault test" OFF)
if(TURBO_MEDIA_TEST_FAULT_INJECTION)
  target_compile_definitions(turbo_media_streamer PRIVATE
                             TURBO_MEDIA_HTTP_FLV_TEST_HOOKS)
endif()
