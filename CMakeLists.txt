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

function(turbo_media_find_tbe_compiler output_variable)
  if(CMAKE_CROSSCOMPILING)
    set(_turbomedia_tbe_root_name TURBOPARSER_HOST_ROOT)
  else()
    set(_turbomedia_tbe_root_name TURBOPARSER_ROOT)
  endif()
  if(NOT DEFINED ENV{${_turbomedia_tbe_root_name}} OR
     "$ENV{${_turbomedia_tbe_root_name}}" STREQUAL "" OR
     NOT IS_DIRECTORY "$ENV{${_turbomedia_tbe_root_name}}")
    message(FATAL_ERROR
            "${_turbomedia_tbe_root_name} must name the TurboParser host install prefix")
  endif()

  file(REAL_PATH "$ENV{${_turbomedia_tbe_root_name}}"
       _turbomedia_tbe_root_real)
  unset(${output_variable} CACHE)
  unset(${output_variable})
  find_program(${output_variable} NAMES tbe_compiler tbe_compiler.exe
               PATHS "${_turbomedia_tbe_root_real}/bin" NO_DEFAULT_PATH)
  if(NOT ${output_variable})
    message(FATAL_ERROR
            "tbe_compiler was not found under ${_turbomedia_tbe_root_real}/bin")
  endif()
  file(REAL_PATH "${${output_variable}}" _turbomedia_tbe_compiler_real)
  cmake_path(IS_PREFIX _turbomedia_tbe_root_real
             "${_turbomedia_tbe_compiler_real}" NORMALIZE
             _turbomedia_tbe_compiler_in_root)
  if(NOT _turbomedia_tbe_compiler_in_root)
    message(FATAL_ERROR
            "tbe_compiler resolved outside ${_turbomedia_tbe_root_name}: ${_turbomedia_tbe_compiler_real}")
  endif()
  set(${output_variable} "${_turbomedia_tbe_compiler_real}" PARENT_SCOPE)
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
  install(
    TARGETS ${target_name}
    EXPORT TurboMediaTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endfunction()

# External imported targets are not copied by vcpkg's applocal step. Keep
# executable/test bundles runnable from a clean build directory on Windows.
function(turbo_media_deploy_windows_runtime target_name)
  if(WIN32)
    foreach(runtime_target IN LISTS ARGN)
      if(TARGET ${runtime_target})
        add_custom_command(
          TARGET ${target_name}
          POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  $<TARGET_FILE:${runtime_target}>
                  $<TARGET_FILE_DIR:${target_name}>
          COMMENT "Deploying ${runtime_target} for ${target_name}")
      endif()
    endforeach()
  endif()
endfunction()

add_subdirectory(tools)
include(FindTools)

foreach(_turbomedia_dependency_root IN ITEMS
        TURBOUTILS_ROOT TURBOPARSER_ROOT TURBONET_ROOT TURBOHTTP_ROOT
        FLOWMQ_ROOT TURBODB_ROOT RULES_FORGE_ROOT)
  if(NOT DEFINED ENV{${_turbomedia_dependency_root}} OR
     "$ENV{${_turbomedia_dependency_root}}" STREQUAL "")
    message(FATAL_ERROR
            "Required dependency root is not defined: ${_turbomedia_dependency_root}")
  endif()
  if(NOT IS_DIRECTORY "$ENV{${_turbomedia_dependency_root}}")
    message(FATAL_ERROR
            "${_turbomedia_dependency_root} is not a directory: $ENV{${_turbomedia_dependency_root}}")
  endif()
endforeach()

find_package(Threads REQUIRED)
find_package(TurboUtils CONFIG REQUIRED
             PATHS "$ENV{TURBOUTILS_ROOT}" NO_DEFAULT_PATH)
find_package(TurboParser CONFIG REQUIRED
             PATHS "$ENV{TURBOPARSER_ROOT}" NO_DEFAULT_PATH)
# RulesForge releases installed before its DataBind migration still export the
# former target name in their link interface. Keep that packaging detail at the
# build boundary; production code links TurboParser::DataBind directly.
if(TARGET TurboParser::DataBind AND NOT TARGET TurboUtils::DataBind)
  add_library(TurboUtils::DataBind ALIAS TurboParser::DataBind)
endif()
find_package(TurboNet CONFIG REQUIRED
             PATHS "$ENV{TURBONET_ROOT}" NO_DEFAULT_PATH)
find_package(TurboHttp CONFIG REQUIRED
             PATHS "$ENV{TURBOHTTP_ROOT}" NO_DEFAULT_PATH)
find_package(RulesForge CONFIG REQUIRED
             PATHS "$ENV{RULES_FORGE_ROOT}" NO_DEFAULT_PATH)
find_package(FlowMQ CONFIG REQUIRED
             PATHS "$ENV{FLOWMQ_ROOT}" NO_DEFAULT_PATH)
find_package(TurboDB CONFIG REQUIRED
             PATHS "$ENV{TURBODB_ROOT}" NO_DEFAULT_PATH)
 
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
