cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(
  TurboMedia
  VERSION 1.0.0
  LANGUAGES C CXX)

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
find_package(TurboUtils CONFIG REQUIRED)
find_package(TurboNet CONFIG REQUIRED)
find_package(TurboHttp CONFIG REQUIRED)
if(NOT TARGET TurboNet::Ice)
  message(FATAL_ERROR "TurboNet package does not provide the required Ice component")
endif()
if(NOT TARGET TurboNet::ICE)
  add_library(TurboNet::ICE INTERFACE IMPORTED)
  set_property(TARGET TurboNet::ICE PROPERTY
               INTERFACE_LINK_LIBRARIES TurboNet::Ice)
endif()
find_package(OpenSSL REQUIRED)
find_package(libSRTP CONFIG REQUIRED)
find_package(unofficial-usrsctp CONFIG REQUIRED)
find_package(roaring CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)

find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)
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

if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  # The base X11 library is implicit in FindX11. Treating it as a component
  # breaks configuration with CMake 3.25 even when libX11 is installed.
  find_package(X11 REQUIRED COMPONENTS Xext)
endif()

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
add_subdirectory(server)
add_subdirectory(streamer)
add_subdirectory(pipeline)
add_subdirectory(media)
add_subdirectory(player)
add_subdirectory(recognition)
add_subdirectory(media/rtsp)
target_sources(turbo_media_server PRIVATE server/rtsp_server_adapter.c)
target_link_libraries(turbo_media_server PUBLIC TurboMedia::RTSP)
add_subdirectory(media/mobile)

add_subdirectory(webrtc)
add_subdirectory(examples)
add_subdirectory(tests)

install(
  DIRECTORY core/include/ crypto/include/ speech/include/ recognition/include/ server/include/
             media/include/ muxer/include/ demuxer/include/ network/include/ streamer/include/
             pipeline/include/ webrtc/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(
  FILES transport/rtp/include/rtp-header.h
        transport/rtp/include/rtp-packet.h
        transport/rtp/include/rtp-payload.h
        media/rtsp/parser/turbo_rtsp_parser.h
        media/rtsp/parser/turbo_rtsp_message.h
        media/rtsp/parser/turbo_rtsp_rtp.h
        media/rtsp/parser/turbo_rtsp_sdp.h
        media/mobile/turbo_mobile.h
        media/mobile/turbo_mobile_codec.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

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
  COMPATIBILITY AnyNewerVersion)

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
