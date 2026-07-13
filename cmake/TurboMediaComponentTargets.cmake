function(turbo_media_configure_component target_name export_name)
  target_include_directories(
    ${target_name}
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/media/include>
           $<INSTALL_INTERFACE:include>)

  target_compile_definitions(${target_name} PRIVATE SHARED_CXX)

  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /wd4100 /wd4996)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()

  set_target_properties(
    ${target_name}
    PROPERTIES FOLDER "turbomedia/components"
               EXPORT_NAME ${export_name})
endfunction()

function(turbo_media_add_component_alias target_name alias_name)
  if(NOT TARGET ${alias_name})
    add_library(${alias_name} ALIAS ${target_name})
  endif()
endfunction()

function(turbo_media_component_warn_missing feature source_path)
  if(NOT EXISTS "${source_path}")
    message(WARNING "${feature} component source is missing: ${source_path}")
  endif()
endfunction()

add_library(turbo_media_common_objects OBJECT)
target_sources(
  turbo_media_common_objects
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common/flv/turbo_flv_header.c
          ${CMAKE_CURRENT_SOURCE_DIR}/common/mpeg/turbo_mpeg_util.c)

file(GLOB TURBO_MEDIA_CODEC_HELPER_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/common/codec_helpers/*.c)
target_sources(turbo_media_common_objects PRIVATE ${TURBO_MEDIA_CODEC_HELPER_SOURCES})

target_include_directories(
  turbo_media_common_objects
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/media/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/flv>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/mpeg>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/codec_helpers>)

if(TURBO_MEDIA_ENABLE_MP4)
  file(GLOB TURBO_MEDIA_MOV_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/common/mov/*.c)
  target_sources(turbo_media_common_objects PRIVATE ${TURBO_MEDIA_MOV_COMMON_SOURCES})
  target_include_directories(
    turbo_media_common_objects
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/mov>)
endif()

if(TURBO_MEDIA_ENABLE_MKV)
  file(GLOB TURBO_MEDIA_MKV_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/common/mkv/*.c)
  target_sources(turbo_media_common_objects PRIVATE ${TURBO_MEDIA_MKV_COMMON_SOURCES})
  target_include_directories(
    turbo_media_common_objects
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/mkv>)
endif()

set_target_properties(turbo_media_common_objects PROPERTIES FOLDER "turbomedia/components")

add_library(turbo_media_core SHARED)
turbo_media_configure_component(turbo_media_core Core)
turbo_media_add_component_alias(turbo_media_core TurboMedia::Core)
target_include_directories(
  turbo_media_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
         $<INSTALL_INTERFACE:include>)
target_sources(turbo_media_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/media_source.c)

add_library(turbo_media_codec SHARED)
turbo_media_configure_component(turbo_media_codec Codec)
turbo_media_add_component_alias(turbo_media_codec TurboMedia::Codec)
target_sources(
  turbo_media_codec
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/codec/codec_registry.c
          ${CMAKE_CURRENT_SOURCE_DIR}/codec/g711_codec.c)

if(TURBO_MEDIA_ENABLE_OPUS)
  target_sources(turbo_media_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/codec/opus_codec.c)
  target_include_directories(turbo_media_codec PRIVATE ${OPUS_INCLUDE_DIR})
  target_link_libraries(turbo_media_codec PRIVATE ${OPUS_LIBRARY})
  target_compile_definitions(turbo_media_codec PUBLIC TURBO_MEDIA_HAS_OPUS)
endif()

if(TURBO_MEDIA_ENABLE_VPX)
  target_sources(turbo_media_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/codec/vpx_codec.c)
  target_include_directories(turbo_media_codec PRIVATE ${VPX_INCLUDE_DIR})
  target_link_libraries(turbo_media_codec PRIVATE ${VPX_LIBRARY})
  target_compile_definitions(turbo_media_codec PUBLIC TURBO_MEDIA_HAS_VPX)
endif()

if(TURBO_MEDIA_ENABLE_H264)
  target_sources(turbo_media_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/codec/h264_codec.c)
  target_include_directories(turbo_media_codec PRIVATE ${OPENH264_INCLUDE_DIR})
  target_link_libraries(turbo_media_codec PRIVATE ${OPENH264_LIBRARY})
  target_compile_definitions(turbo_media_codec PUBLIC TURBO_MEDIA_HAS_H264)
endif()

if(TURBO_MEDIA_ENABLE_H265)
  target_sources(turbo_media_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/codec/h265_codec.c)
  target_include_directories(turbo_media_codec PRIVATE ${X265_INCLUDE_DIR} ${LIBDE265_INCLUDE_DIR})
  target_link_libraries(turbo_media_codec PRIVATE ${X265_LIBRARY} ${LIBDE265_LIBRARY})
  target_compile_definitions(turbo_media_codec PUBLIC TURBO_MEDIA_HAS_H265)
endif()

add_library(turbo_media_muxer SHARED)
turbo_media_configure_component(turbo_media_muxer Muxer)
turbo_media_add_component_alias(turbo_media_muxer TurboMedia::Muxer)
target_link_libraries(turbo_media_muxer PUBLIC turbo_media_codec PRIVATE turbo_media_common_objects)
target_include_directories(
  turbo_media_muxer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/muxer/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/flv
          ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mov
          ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mkv
          ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpeg)
target_sources(turbo_media_muxer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/muxer_registry.c)

if(TURBO_MEDIA_ENABLE_FLV)
  target_sources(
    turbo_media_muxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/flv_muxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/flv/flv_muxer_impl.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/flv/flv_writer.c)
  target_compile_definitions(turbo_media_muxer PUBLIC TURBO_MEDIA_HAS_FLV)
endif()

if(TURBO_MEDIA_ENABLE_MP4)
  turbo_media_component_warn_missing("MP4 muxer" "${CMAKE_CURRENT_SOURCE_DIR}/muxer/mp4_muxer.c")
  target_sources(
    turbo_media_muxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mp4_muxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mov/mov-writer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mov/fmp4-writer.c)
  target_compile_definitions(turbo_media_muxer PUBLIC TURBO_MEDIA_HAS_MP4)
endif()

if(TURBO_MEDIA_ENABLE_MKV)
  turbo_media_component_warn_missing("MKV muxer" "${CMAKE_CURRENT_SOURCE_DIR}/muxer/mkv_muxer.c")
  target_sources(
    turbo_media_muxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mkv_muxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mkv/mkv-writer.c)
  target_compile_definitions(turbo_media_muxer PUBLIC TURBO_MEDIA_HAS_MKV)
endif()

if(TURBO_MEDIA_ENABLE_MPEG)
  turbo_media_component_warn_missing("MPEG-TS muxer" "${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpegts_muxer.c")
  turbo_media_component_warn_missing("MPEG-PS muxer" "${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpegps_muxer.c")
  target_sources(
    turbo_media_muxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpegts_muxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpegps_muxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpeg/mpeg-ts-enc.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpeg/mpeg-ps-enc.c
            ${CMAKE_CURRENT_SOURCE_DIR}/muxer/mpeg/mpeg-element-descriptor.c)
  target_compile_definitions(turbo_media_muxer PUBLIC TURBO_MEDIA_HAS_MPEG)
endif()

add_library(turbo_media_demuxer SHARED)
turbo_media_configure_component(turbo_media_demuxer Demuxer)
turbo_media_add_component_alias(turbo_media_demuxer TurboMedia::Demuxer)
target_link_libraries(turbo_media_demuxer PUBLIC turbo_media_codec turbo_media_muxer PRIVATE turbo_media_common_objects)
target_include_directories(
  turbo_media_demuxer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/demuxer/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv
          ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mov
          ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mkv
          ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpeg)
target_sources(turbo_media_demuxer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/demuxer_registry.c)

if(TURBO_MEDIA_ENABLE_FLV)
  target_sources(
    turbo_media_demuxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv_demuxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv/flv_demuxer_impl.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv/flv_demuxer_script.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv/flv_reader.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/flv/flv_parser.c)
  target_compile_definitions(turbo_media_demuxer PUBLIC TURBO_MEDIA_HAS_FLV)
endif()

if(TURBO_MEDIA_ENABLE_MP4)
  turbo_media_component_warn_missing("MP4 demuxer" "${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mp4_demuxer.c")
  target_sources(
    turbo_media_demuxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mp4_demuxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mov/mov-reader.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mov/fmp4-reader.c)
  target_compile_definitions(turbo_media_demuxer PUBLIC TURBO_MEDIA_HAS_MP4)
endif()

if(TURBO_MEDIA_ENABLE_MKV)
  turbo_media_component_warn_missing("MKV demuxer" "${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mkv_demuxer.c")
  target_sources(
    turbo_media_demuxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mkv_demuxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mkv/mkv-reader.c)
  target_compile_definitions(turbo_media_demuxer PUBLIC TURBO_MEDIA_HAS_MKV)
endif()

if(TURBO_MEDIA_ENABLE_MPEG)
  turbo_media_component_warn_missing("MPEG-TS demuxer" "${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpegts_demuxer.c")
  turbo_media_component_warn_missing("MPEG-PS demuxer" "${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpegps_demuxer.c")
  target_sources(
    turbo_media_demuxer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpegts_demuxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpegps_demuxer.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpeg/mpeg-ts-dec.c
            ${CMAKE_CURRENT_SOURCE_DIR}/demuxer/mpeg/mpeg-ps-dec.c)
  target_compile_definitions(turbo_media_demuxer PUBLIC TURBO_MEDIA_HAS_MPEG)
endif()

add_library(turbo_media_transport SHARED)
turbo_media_configure_component(turbo_media_transport Transport)
turbo_media_add_component_alias(turbo_media_transport TurboMedia::Transport)
target_include_directories(
  turbo_media_transport
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/network/include>
         $<INSTALL_INTERFACE:include>)
target_sources(turbo_media_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network/transport_coronet.c)

if(TARGET TurboHTTP::http_client)
  target_sources(turbo_media_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/network/transport_http.c)
  target_link_libraries(turbo_media_transport PUBLIC TurboHTTP::http_client)
endif()

find_package(Threads QUIET)
if(TARGET TurboNet::CoroNet)
  target_link_libraries(turbo_media_transport PUBLIC TurboNet::CoroNet TurboUtils::Parser)
else()
  set(TURBO_MEDIA_TURBONET_HINTS)
  if(TURBONET_ROOT)
    list(APPEND TURBO_MEDIA_TURBONET_HINTS "${TURBONET_ROOT}")
  endif()
  list(APPEND TURBO_MEDIA_TURBONET_HINTS
       "${PROJECT_SOURCE_DIR}/../turbonet/build"
       "${PROJECT_SOURCE_DIR}/../turbonet")
  find_package(TurboNet QUIET PATHS ${TURBO_MEDIA_TURBONET_HINTS})
  if(TurboNet_FOUND)
    target_link_libraries(turbo_media_transport PUBLIC TurboNet::CoroNet TurboUtils::Parser)
  endif()
endif()

if(TURBO_MEDIA_ENABLE_RTP)
  file(GLOB TURBO_MEDIA_RTP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/*.c)
  file(GLOB TURBO_MEDIA_RTP_EXT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/rtpext/*.c)
  file(GLOB TURBO_MEDIA_RTP_PAYLOAD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/payload/*.c)
  target_sources(
    turbo_media_transport
    PRIVATE ${TURBO_MEDIA_RTP_SOURCES}
            ${TURBO_MEDIA_RTP_EXT_SOURCES}
            ${TURBO_MEDIA_RTP_PAYLOAD_SOURCES})
  target_include_directories(
    turbo_media_transport
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp
           ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/include
           ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/rtpext
           ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtp/payload)
  target_compile_definitions(turbo_media_transport PUBLIC TURBO_MEDIA_HAS_RTP)
endif()

if(TURBO_MEDIA_ENABLE_LEGACY_RTSP_TRANSPORT)
  file(GLOB_RECURSE TURBO_MEDIA_RTSP_TRANSPORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/transport/rtsp/*.c)
  target_sources(turbo_media_transport PRIVATE ${TURBO_MEDIA_RTSP_TRANSPORT_SOURCES})
  target_include_directories(
    turbo_media_transport
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/transport/rtsp>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/transport/rtsp/include>
           $<INSTALL_INTERFACE:include/turbomedia/transport/rtsp>
           $<INSTALL_INTERFACE:include/turbomedia/transport/rtsp/include>)
  target_compile_definitions(turbo_media_transport PUBLIC TURBO_MEDIA_HAS_RTSP)
endif()

if(TURBO_MEDIA_ENABLE_SIP)
  file(GLOB_RECURSE TURBO_MEDIA_SIP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/transport/sip/*.c)
  target_sources(turbo_media_transport PRIVATE ${TURBO_MEDIA_SIP_SOURCES})
  target_include_directories(
    turbo_media_transport
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/transport/sip
           ${CMAKE_CURRENT_SOURCE_DIR}/transport/sip/include)
  target_compile_definitions(turbo_media_transport PUBLIC TURBO_MEDIA_HAS_SIP)
endif()

add_library(turbo_media_server SHARED)
turbo_media_configure_component(turbo_media_server Server)
turbo_media_add_component_alias(turbo_media_server TurboMedia::Server)
target_link_libraries(turbo_media_server PUBLIC turbo_media_core turbo_media_transport)
target_include_directories(
  turbo_media_server
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
         $<INSTALL_INTERFACE:include>)
target_sources(turbo_media_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server/server_runtime.c)

add_library(turbo_media_streamer SHARED)
turbo_media_configure_component(turbo_media_streamer Streamer)
turbo_media_add_component_alias(turbo_media_streamer TurboMedia::Streamer)
target_link_libraries(
  turbo_media_streamer
  PUBLIC turbo_media_server turbo_media_codec turbo_media_muxer turbo_media_demuxer turbo_media_transport)
target_include_directories(
  turbo_media_streamer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/streamer/include>
         $<INSTALL_INTERFACE:include>)
target_sources(turbo_media_streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/streamer_registry.c)

if(TURBO_MEDIA_ENABLE_HLS)
  file(GLOB TURBO_MEDIA_HLS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/streamer/hls/*.c)
  target_sources(
    turbo_media_streamer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/hls_streamer.c
            ${TURBO_MEDIA_HLS_SOURCES})
  target_include_directories(turbo_media_streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/hls)
  target_compile_definitions(turbo_media_streamer PUBLIC TURBO_MEDIA_HAS_HLS)
  if(TARGET TurboHTTP::http_client)
    target_link_libraries(turbo_media_streamer PRIVATE TurboHTTP::http_client)
  endif()
endif()

if(TURBO_MEDIA_ENABLE_DASH)
  turbo_media_component_warn_missing("DASH streamer" "${CMAKE_CURRENT_SOURCE_DIR}/streamer/dash_streamer.c")
  file(GLOB TURBO_MEDIA_DASH_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/streamer/dash/*.c)
  target_sources(
    turbo_media_streamer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/dash_streamer.c
            ${TURBO_MEDIA_DASH_SOURCES})
  target_include_directories(turbo_media_streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/dash)
  target_compile_definitions(turbo_media_streamer PUBLIC TURBO_MEDIA_HAS_DASH)
  if(TARGET TurboHTTP::http_client)
    target_link_libraries(turbo_media_streamer PRIVATE TurboHTTP::http_client)
  endif()
endif()

if(TURBO_MEDIA_ENABLE_RTMP)
  file(GLOB TURBO_MEDIA_RTMP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp/*.c)
  file(GLOB TURBO_MEDIA_RTMP_AIO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp/aio/*.c)
  target_sources(
    turbo_media_streamer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp_streamer.c
            ${TURBO_MEDIA_RTMP_SOURCES}
            ${TURBO_MEDIA_RTMP_AIO_SOURCES})
  target_include_directories(
    turbo_media_streamer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp
            ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp/include
            ${CMAKE_CURRENT_SOURCE_DIR}/streamer/rtmp/aio)
  target_compile_definitions(turbo_media_streamer PUBLIC TURBO_MEDIA_HAS_RTMP)
endif()

if(TURBO_MEDIA_ENABLE_HTTP_FLV)
  turbo_media_component_warn_missing("HTTP-FLV streamer" "${CMAKE_CURRENT_SOURCE_DIR}/streamer/http_flv_streamer.c")
  target_sources(turbo_media_streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/streamer/http_flv_streamer.c)
  target_compile_definitions(turbo_media_streamer PUBLIC TURBO_MEDIA_HAS_HTTP_FLV)
endif()

add_library(turbo_media_device SHARED)
turbo_media_configure_component(turbo_media_device Device)
turbo_media_add_component_alias(turbo_media_device TurboMedia::Device)
target_link_libraries(turbo_media_device PRIVATE yuv)
target_include_directories(turbo_media_device PRIVATE ${MINIAUDIO_INCLUDE_DIRS} ${Stb_INCLUDE_DIR})
target_sources(
  turbo_media_device
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/media/playback/miniaudio_impl.c
          ${CMAKE_CURRENT_SOURCE_DIR}/media/playback/turbo_playback.c
          ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_audio_miniaudio.c)

if(WIN32)
  target_sources(
    turbo_media_device
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_win32.c
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_video_win32.c
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_screen_win32.c)
  target_link_libraries(
    turbo_media_device
    PRIVATE mfplat
            mf
            mfuuid
            mfreadwrite
            strmiids
            ole32
            oleaut32
            d3d11
            dxgi
            dxguid)
elseif(APPLE)
  target_sources(
    turbo_media_device
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_macos.c
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_video_macos.m
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_screen_macos.m)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
  target_link_libraries(
    turbo_media_device
    PRIVATE ${AVFOUNDATION_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
            ${COREGRAPHICS_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK})
else()
  target_sources(
    turbo_media_device
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_linux.c
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_video_linux.c
            ${CMAKE_CURRENT_SOURCE_DIR}/media/capture/capture_screen_linux.c)
  target_compile_definitions(turbo_media_device PRIVATE TURBO_HAS_PIPEWIRE)
  target_link_libraries(turbo_media_device PRIVATE Threads::Threads PkgConfig::PIPEWIRE)
  if(X11_FOUND AND X11_Xext_FOUND)
    target_compile_definitions(turbo_media_device PRIVATE TURBO_HAS_X11)
    target_link_libraries(turbo_media_device PRIVATE X11::X11 X11::Xext)
  endif()
endif()

if(TURBO_MEDIA_ENABLE_FFMPEG)
  find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h REQUIRED)
  find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat REQUIRED)
  find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec REQUIRED)
  find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil REQUIRED)
  find_library(FFMPEG_SWRESAMPLE_LIBRARY NAMES swresample REQUIRED)
  find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale REQUIRED)

  add_library(turbo_media_player SHARED)
  turbo_media_configure_component(turbo_media_player Player)
  turbo_media_add_component_alias(turbo_media_player TurboMedia::Player)
  target_sources(turbo_media_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/player/ffmpeg_player.c)
  target_link_libraries(turbo_media_player PUBLIC turbo_media_codec turbo_media_demuxer turbo_media_device)
  target_include_directories(turbo_media_player PRIVATE ${FFMPEG_INCLUDE_DIR})
  target_link_libraries(
    turbo_media_player
    PRIVATE ${FFMPEG_AVFORMAT_LIBRARY}
            ${FFMPEG_AVCODEC_LIBRARY}
            ${FFMPEG_AVUTIL_LIBRARY}
            ${FFMPEG_SWRESAMPLE_LIBRARY}
            ${FFMPEG_SWSCALE_LIBRARY})
  target_compile_definitions(turbo_media_player PUBLIC TURBO_MEDIA_HAS_FFMPEG)
endif()
