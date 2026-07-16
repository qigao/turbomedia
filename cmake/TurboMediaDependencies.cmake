include(GNUInstallDirs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(TURBO_MEDIA_TURBOUTILS_HINTS)
if(TURBOUTILS_ROOT)
  list(APPEND TURBO_MEDIA_TURBOUTILS_HINTS "${TURBOUTILS_ROOT}")
endif()
list(APPEND TURBO_MEDIA_TURBOUTILS_HINTS
     "C:/projects/cpp/external/pkgs/turboutils"
     "${PROJECT_SOURCE_DIR}/../turbo-utils/build")
find_package(TurboUtils CONFIG REQUIRED HINTS ${TURBO_MEDIA_TURBOUTILS_HINTS})

if(TURBO_MEDIA_ENABLE_WEBRTC)
  find_package(OpenSSL REQUIRED)
  find_package(unofficial-usrsctp CONFIG REQUIRED)
  find_package(roaring CONFIG REQUIRED)
  find_package(libSRTP CONFIG QUIET)
  if(NOT TARGET libSRTP::srtp2)
    find_package(libsrtp CONFIG REQUIRED)
  endif()
  if(NOT TARGET libSRTP::srtp2)
    message(FATAL_ERROR
            "TURBO_MEDIA_ENABLE_WEBRTC requires libSRTP::srtp2")
  endif()

  set(TURBO_MEDIA_TURBONET_HINTS)
  if(TURBONET_ROOT)
    list(APPEND TURBO_MEDIA_TURBONET_HINTS "${TURBONET_ROOT}")
  endif()
  find_package(TurboNet CONFIG REQUIRED HINTS ${TURBO_MEDIA_TURBONET_HINTS})
  if(NOT TARGET TurboNet::Ice)
    message(FATAL_ERROR
            "TURBO_MEDIA_ENABLE_WEBRTC requires a TurboNet package exporting TurboNet::Ice")
  endif()

  set(TURBO_MEDIA_TURBOHTTP_HINTS)
  if(TURBOHTTP_ROOT)
    list(APPEND TURBO_MEDIA_TURBOHTTP_HINTS "${TURBOHTTP_ROOT}")
  endif()
  list(APPEND TURBO_MEDIA_TURBOHTTP_HINTS
       "C:/projects/cpp/external/pkgs/turbohttp"
       "${PROJECT_SOURCE_DIR}/../../TurboHTTP/build")
  find_package(TurboHttp CONFIG REQUIRED HINTS ${TURBO_MEDIA_TURBOHTTP_HINTS})
  if(NOT TARGET TurboHttp::Iris)
    message(FATAL_ERROR
            "TURBO_MEDIA_ENABLE_WEBRTC requires a TurboHttp package exporting TurboHttp::Iris")
  endif()
endif()

find_path(MINIAUDIO_INCLUDE_DIRS NAMES miniaudio.h REQUIRED)
find_path(Stb_INCLUDE_DIR NAMES stb_sprintf.h stb_image_write.h REQUIRED)
find_package(libyuv CONFIG REQUIRED)

if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  find_package(X11)
endif()
