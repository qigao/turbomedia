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
endif()

find_path(MINIAUDIO_INCLUDE_DIRS NAMES miniaudio.h REQUIRED)
find_path(Stb_INCLUDE_DIR NAMES stb_sprintf.h stb_image_write.h REQUIRED)
find_package(libyuv CONFIG REQUIRED)

if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  find_package(X11)
endif()
