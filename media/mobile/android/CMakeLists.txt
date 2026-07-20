set(TURBO_MEDIA_MOBILE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(TURBO_MEDIA_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_library(TURBO_MEDIA_ANDROID_LOG_LIBRARY log REQUIRED)
find_library(TURBO_MEDIA_ANDROID_LIBRARY android REQUIRED)
find_library(TURBO_MEDIA_ANDROID_OPENSL_LIBRARY OpenSLES REQUIRED)
find_library(TURBO_MEDIA_ANDROID_MEDIA_LIBRARY mediandk REQUIRED)
find_library(TURBO_MEDIA_ANDROID_CAMERA_LIBRARY camera2ndk REQUIRED)
find_library(TURBO_MEDIA_ANDROID_JNIGRAPHICS_LIBRARY jnigraphics REQUIRED)

add_library(
  turbo_media_android SHARED
  src/jni/turbo_media_jni.c
  src/codec/hardware_codec_android.c
  src/mobile/mobile_optimizations.c
  src/mobile/battery_monitor.c
  src/mobile/network_monitor.c)
turbo_media_configure_component(turbo_media_android Android)
target_include_directories(
  turbo_media_android
  PUBLIC $<BUILD_INTERFACE:${TURBO_MEDIA_MOBILE_ROOT}>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src ${TURBO_MEDIA_ROOT}/playback ${MINIAUDIO_INCLUDE_DIR})
target_compile_definitions(turbo_media_android PRIVATE TURBO_MEDIA_ANDROID=1)
target_compile_options(
  turbo_media_android
  PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
target_link_libraries(
  turbo_media_android
  PUBLIC TurboMedia::Device
  PRIVATE ${TURBO_MEDIA_ANDROID_LOG_LIBRARY}
          ${TURBO_MEDIA_ANDROID_LIBRARY}
          ${TURBO_MEDIA_ANDROID_OPENSL_LIBRARY}
          ${TURBO_MEDIA_ANDROID_MEDIA_LIBRARY}
          ${TURBO_MEDIA_ANDROID_CAMERA_LIBRARY}
          ${TURBO_MEDIA_ANDROID_JNIGRAPHICS_LIBRARY})
