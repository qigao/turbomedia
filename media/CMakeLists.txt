add_library(
  turbo_media_device SHARED
  playback/miniaudio_impl.c
  playback/turbo_playback.c)
turbo_media_configure_component(turbo_media_device Device)
target_include_directories(
  turbo_media_device
  PRIVATE ${MINIAUDIO_INCLUDE_DIR} ${STB_INCLUDE_DIR})
target_link_libraries(turbo_media_device PRIVATE yuv)

if(ANDROID)
  target_sources(
    turbo_media_device
    PRIVATE mobile/android/src/capture/capture_android.c
            mobile/android/src/capture/capture_video_android.c
            mobile/android/src/capture/capture_audio_android.c
            mobile/android/src/capture/capture_screen_android.c)
  find_library(TURBO_MEDIA_ANDROID_LOG_LIBRARY log REQUIRED)
  find_library(TURBO_MEDIA_ANDROID_LIBRARY android REQUIRED)
  find_library(TURBO_MEDIA_ANDROID_OPENSL_LIBRARY OpenSLES REQUIRED)
  find_library(TURBO_MEDIA_ANDROID_MEDIA_LIBRARY mediandk REQUIRED)
  find_library(TURBO_MEDIA_ANDROID_CAMERA_LIBRARY camera2ndk REQUIRED)
  target_link_libraries(
    turbo_media_device
    PRIVATE ${TURBO_MEDIA_ANDROID_LOG_LIBRARY}
            ${TURBO_MEDIA_ANDROID_LIBRARY}
            ${TURBO_MEDIA_ANDROID_OPENSL_LIBRARY}
            ${TURBO_MEDIA_ANDROID_MEDIA_LIBRARY}
            ${TURBO_MEDIA_ANDROID_CAMERA_LIBRARY}
            ${CMAKE_DL_LIBS})
else()
  target_sources(turbo_media_device PRIVATE capture/capture_audio_miniaudio.c)
endif()

if(WIN32)
  target_sources(
    turbo_media_device
    PRIVATE capture/capture_win32.c
            capture/capture_video_win32.c
            capture/capture_screen_win32.c)
  target_link_libraries(
    turbo_media_device
    PRIVATE mfplat mf mfuuid mfreadwrite strmiids ole32 oleaut32 d3d11 dxgi dxguid)
elseif(APPLE)
  target_sources(
    turbo_media_device
    PRIVATE capture/capture_macos.c
            capture/capture_video_macos.m
            capture/capture_screen_macos.m)
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
elseif(NOT ANDROID)
  target_sources(
    turbo_media_device
    PRIVATE capture/capture_linux.c
            capture/capture_video_linux.c
            capture/capture_screen_linux.c)
  target_compile_definitions(turbo_media_device PRIVATE TURBO_HAS_PIPEWIRE TURBO_HAS_X11)
  target_link_libraries(
    turbo_media_device
    PRIVATE Threads::Threads PkgConfig::PIPEWIRE X11::X11 X11::Xext)
endif()
