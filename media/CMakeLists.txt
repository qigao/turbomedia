target_sources(
  turbo_media
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/playback/miniaudio_impl.c
          ${CMAKE_CURRENT_LIST_DIR}/playback/turbo_playback.c)

target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/capture/capture_audio_miniaudio.c)

if(WIN32)
  target_sources(
    turbo_media
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/capture/capture_win32.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_video_win32.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_screen_win32.c)
  target_link_libraries(
    turbo_media
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
    turbo_media
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/capture/capture_macos.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_video_macos.m
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_screen_macos.m)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
  target_link_libraries(
    turbo_media
    PRIVATE ${AVFOUNDATION_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
            ${COREGRAPHICS_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK})
else()
  target_sources(
    turbo_media
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/capture/capture_linux.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_video_linux.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/capture_screen_linux.c)
  target_compile_definitions(turbo_media PRIVATE TURBO_HAS_PIPEWIRE)
  target_link_libraries(turbo_media PRIVATE Threads::Threads PkgConfig::PIPEWIRE)
  if(X11_FOUND AND X11_Xext_FOUND)
    target_compile_definitions(turbo_media PRIVATE TURBO_HAS_X11)
    target_link_libraries(turbo_media PRIVATE X11::X11 X11::Xext)
  endif()
endif()

if(TURBO_MEDIA_ENABLE_RTSP)
  include(${CMAKE_CURRENT_LIST_DIR}/rtsp/CMakeLists.txt)
endif()

if(BUILD_MOBILE)
  add_subdirectory(media/mobile)
endif()
