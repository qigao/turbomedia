cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(
  TurboMedia
  VERSION 2.0.0
  LANGUAGES C CXX)

option(TM_M15D_IOCP_KERNEL_ONLY
       "Build only the real RTSP/CNet and private IOCP conformance graph" OFF)
if(TM_M15D_IOCP_KERNEL_ONLY AND
   (NOT WIN32 OR CMAKE_CROSSCOMPILING OR NOT TM_M15D_IOCP_KERNEL_SOURCE_ROOT))
  message(FATAL_ERROR "Focused IOCP qualification requires native Windows and exact private source")
endif()

# Server/service targets are a desktop platform capability, not a product
# profile selected by the caller.
set(TURBO_MEDIA_HAS_DESKTOP_SERVICES OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR
   CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(TURBO_MEDIA_HAS_DESKTOP_SERVICES ON)
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
include(TurboMediaRuntimeInstall)
enable_testing()

include(FindTools)

find_package(Threads REQUIRED)
if(WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(JPEG REQUIRED)
endif()

find_package(SaltsBase "$ENV{SALTS_BASE_VERSION}" EXACT CONFIG REQUIRED
             PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta "$ENV{CMETA_VERSION}" EXACT CONFIG REQUIRED
             PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCNet "$ENV{SALTS_CNET_VERSION}" EXACT CONFIG REQUIRED
             PATHS "$ENV{SALTS_CNET_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils "$ENV{SALTS_UTILS_VERSION}" EXACT CONFIG REQUIRED
             PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Chttp "$ENV{CHTTP_VERSION}" EXACT CONFIG REQUIRED
             COMPONENTS Client Server App WebSocketPeerLease
             PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsNet CONFIG REQUIRED
             PATHS "$ENV{SALTSNET_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

find_package(libSRTP CONFIG REQUIRED)
find_package(unofficial-usrsctp CONFIG REQUIRED)
find_package(roaring CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)

find_path(STB_INCLUDE_DIR NAMES stb_sprintf.h stb_image_write.h REQUIRED)
find_path(TURBO_MEDIA_FFMPEG_MODULE_DIR NAMES FindFFMPEG.cmake
          PATH_SUFFIXES share/ffmpeg)
if(TURBO_MEDIA_FFMPEG_MODULE_DIR)
  list(PREPEND CMAKE_MODULE_PATH "${TURBO_MEDIA_FFMPEG_MODULE_DIR}")
endif()
find_package(FFMPEG REQUIRED)
include(TurboMediaFFmpegTargets)

add_subdirectory(streamgraph)
add_subdirectory(media)
add_subdirectory(sip)
add_subdirectory(streaming)
add_subdirectory(webrtc)
add_subdirectory(examples)

install(
  DIRECTORY streamgraph/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaFFmpegTargets.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaFFmpegTargets.cmake"
  COPYONLY)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaCodecLibraries.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaCodecLibraries.cmake"
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
foreach(codec_component IN ITEMS AudioCodec VideoCodec)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaCodecLibraries.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia"
    COMPONENT ${codec_component})
  export(
    EXPORT TurboMedia${codec_component}Targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/TurboMedia${codec_component}Targets.cmake"
    NAMESPACE TurboMedia::)
endforeach()
foreach(protocol_component IN ITEMS SIP RTP SDP RTPICE SIPClient)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia" COMPONENT ${protocol_component})
  export(EXPORT TurboMedia${protocol_component}Targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/TurboMedia${protocol_component}Targets.cmake"
    NAMESPACE TurboMedia::)
endforeach()
foreach(sdk_component IN ITEMS SIP RTP SDP RTPICE SIPClient AudioCodec VideoCodec)
  install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/TurboMedia" COMPONENT ${sdk_component})
endforeach()
export(
  EXPORT TurboMediaTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaTargets.cmake"
  NAMESPACE TurboMedia::)
install(
  EXPORT TurboMediaTargets
  FILE TurboMediaTargets.cmake
  NAMESPACE TurboMedia::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
