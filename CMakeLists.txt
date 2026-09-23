cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/TurboMediaProduct.cmake")
turbo_media_validate_product_target()

project(
  TurboMedia
  VERSION 2.0.0
  LANGUAGES C CXX)

# Validate again after project() so toolchain files that establish the target
# system during language setup cannot bypass the pre-project product gate.
turbo_media_validate_product_target()

set(TURBO_MEDIA_IS_CLIENT OFF)
set(TURBO_MEDIA_IS_SERVER OFF)
if(TURBO_MEDIA_PRODUCT STREQUAL "CLIENT")
  set(TURBO_MEDIA_IS_CLIENT ON)
else()
  set(TURBO_MEDIA_IS_SERVER ON)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(APPLE)
  enable_language(OBJC)
endif()

if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)
include(CmakeUtils)
enable_testing()

function(turbo_media_find_tbe_compiler output_variable)
  if(CMAKE_CROSSCOMPILING)
    set(_turbomedia_tbe_root_name SALTS_UTILS_HOST_ROOT)
  else()
    set(_turbomedia_tbe_root_name SALTS_UTILS_ROOT)
  endif()
  find_program(${output_variable} NAMES tbe_compiler tbe_compiler.exe
               PATHS "$ENV{${_turbomedia_tbe_root_name}}/bin"
               NO_DEFAULT_PATH REQUIRED)
  set(${output_variable} "${${output_variable}}" PARENT_SCOPE)
endfunction()

function(turbo_media_configure_component target_name export_name)
  cmake_config_target(
    ${target_name}
    ALIAS TurboMedia::${export_name}
    EXPORT_NAME ${export_name}
    FOLDER "turbomedia/components")
  target_include_directories(
    ${target_name}
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/media/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  get_target_property(_turbo_media_target_type ${target_name} TYPE)
  if(_turbo_media_target_type STREQUAL "SHARED_LIBRARY" OR
     _turbo_media_target_type STREQUAL "MODULE_LIBRARY")
    target_compile_definitions(${target_name} PRIVATE SHARED_CXX)
  endif()
  unset(_turbo_media_target_type)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /wd4100 /wd4996)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  target_compile_definitions(
    ${target_name} PUBLIC TURBO_MEDIA_PRODUCT_${TURBO_MEDIA_PRODUCT}=1)
  install(
    TARGETS ${target_name}
    EXPORT TurboMediaTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endfunction()

add_subdirectory(tools)
include(FindTools)

find_package(Threads REQUIRED)
find_package(Salts CONFIG REQUIRED
             PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED
             PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(TURBO_MEDIA_IS_CLIENT)
  if(NOT TARGET Salts::Capture OR NOT TARGET Salts::Playback)
    message(FATAL_ERROR
            "TurboMedia CLIENT requires Salts::Capture and Salts::Playback")
  endif()
endif()
find_package(SaltsNet CONFIG REQUIRED
             PATHS "$ENV{SALTSNET_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(TURBO_MEDIA_IS_SERVER)
  find_package(RulesForge CONFIG REQUIRED
               PATHS "$ENV{RULES_FORGE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_package(TurboDB CONFIG REQUIRED
               PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_package(Orm CONFIG REQUIRED
               PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT DEFINED Orm_WITH_SQLITE OR NOT DEFINED Orm_WITH_POSTGRESQL)
    message(FATAL_ERROR
            "TurboMedia SERVER requires an Orm package that exports backend capabilities")
  endif()
  if(Orm_WITH_SQLITE OR NOT Orm_WITH_POSTGRESQL OR NOT TARGET Orm::PostgreSQL)
    message(FATAL_ERROR
            "TurboMedia SERVER requires PostgreSQL-only Orm (SQLite OFF, PostgreSQL ON)")
  endif()
  get_filename_component(TURBO_MEDIA_ORM_PREFIX "${Orm_DIR}/../../.." ABSOLUTE)
endif()
 
find_package(OpenSSL REQUIRED)
find_package(libSRTP CONFIG REQUIRED)
find_package(unofficial-usrsctp CONFIG REQUIRED)
find_package(roaring CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)

find_path(STB_INCLUDE_DIR NAMES stb_sprintf.h stb_image_write.h REQUIRED)
find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY NAMES opus REQUIRED)
find_path(VPX_INCLUDE_DIR NAMES vpx/vpx_encoder.h vpx/vpx_decoder.h REQUIRED)
find_library(VPX_LIBRARY NAMES vpx libvpx REQUIRED)
find_path(OPENH264_INCLUDE_DIR NAMES wels/codec_api.h REQUIRED)
find_library(OPENH264_LIBRARY NAMES openh264 wels REQUIRED)
find_path(X265_INCLUDE_DIR NAMES x265.h REQUIRED)
find_library(X265_LIBRARY NAMES x265 libx265 REQUIRED)
find_path(LIBDE265_INCLUDE_DIR NAMES libde265/de265.h REQUIRED)
find_library(LIBDE265_LIBRARY NAMES de265 libde265 REQUIRED)
find_package(FFMPEG REQUIRED)
include(TurboMediaFFmpegTargets)

add_subdirectory(vendor/monocypher EXCLUDE_FROM_ALL)
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(crypto)
add_subdirectory(speech)
add_subdirectory(codec)
add_subdirectory(muxer)
add_subdirectory(demuxer)
add_subdirectory(network)
add_subdirectory(transport)
if(TURBO_MEDIA_IS_CLIENT)
  add_subdirectory(player)
endif()
add_subdirectory(recognition)
add_subdirectory(media/rtsp)
if(TURBO_MEDIA_IS_CLIENT)
  add_subdirectory(media/mobile)
else()
  add_subdirectory(server)
  add_subdirectory(streamer)
  add_subdirectory(pipeline)
  add_library(turbo_media_server_rtsp SHARED server/rtsp_server_adapter.c)
  turbo_media_configure_component(turbo_media_server_rtsp ServerRTSP)
  target_include_directories(
    turbo_media_server_rtsp
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/server/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(
    turbo_media_server_rtsp PUBLIC TurboMedia::Server TurboMedia::RTSP)
endif()

add_subdirectory(webrtc)
add_subdirectory(examples)
add_subdirectory(tests)

install(
  DIRECTORY core/include/ crypto/include/ speech/include/ recognition/include/
             muxer/include/ demuxer/include/ network/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(
  FILES media/include/turbo_codec.h
        media/include/turbo_export.h
        media/include/turbo_rtsp.h
        transport/rtp/include/rtp-header.h
        transport/rtp/include/rtp-packet.h
        transport/rtp/include/rtp-payload.h
        media/rtsp/parser/turbo_rtsp_parser.h
        media/rtsp/parser/turbo_rtsp_message.h
        media/rtsp/parser/turbo_rtsp_rtp.h
        media/rtsp/parser/turbo_rtsp_sdp.h
        webrtc/include/http_api.h
        webrtc/include/ice_integration.h
        webrtc/include/turbo_datachannel.h
        webrtc/include/turbo_datachannel_errors.h
        webrtc/include/turbo_dc_msg.h
        webrtc/include/turbo_media_engine.h
        webrtc/include/turbo_nack.h
        webrtc/include/turbo_peer_connection.h
        webrtc/include/turbo_rtp.h
        webrtc/include/turbo_sdp.h
        webrtc/include/turbo_simulcast.h
        webrtc/include/turbo_srtp.h
        webrtc/include/turbo_srtp_defs.h
        webrtc/include/webrtc_signaling.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TURBO_MEDIA_IS_CLIENT)
  install(
    FILES media/include/turbo_player.h
          media/mobile/turbo_mobile.h
          media/mobile/turbo_mobile_codec.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  install(
    DIRECTORY server/include/ streamer/include/ pipeline/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
  install(
    FILES webrtc/include/turbo_media_webrtc.h
          webrtc/include/turbo_media_webrtc_backend.h
          webrtc/include/turbo_recorder.h
          webrtc/include/turbo_room_service.h
          webrtc/include/turbo_sfu.h
          webrtc/include/turbo_sfu_node.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaFFmpegTargets.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaFFmpegTargets.cmake"
  COPYONLY)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfigVersion.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaFFmpegTargets.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
export(
  EXPORT TurboMediaTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaTargets.cmake"
  NAMESPACE TurboMedia::)
install(
  EXPORT TurboMediaTargets
  FILE TurboMediaTargets.cmake
  NAMESPACE TurboMedia::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
