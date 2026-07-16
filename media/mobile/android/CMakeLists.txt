cmake_minimum_required(VERSION 3.18.1)

set(TURBO_MEDIA_MOBILE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(TURBO_MEDIA_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(TURBO_MEDIA_ANDROID_ROOT ${CMAKE_CURRENT_LIST_DIR})

set(TURBO_MEDIA_ANDROID_NATIVE_SOURCES
    ${TURBO_MEDIA_MOBILE_ROOT}/turbo_mobile_codec.h
    ${TURBO_MEDIA_MOBILE_ROOT}/turbo_mobile.h
    ${TURBO_MEDIA_ANDROID_ROOT}/src/jni/turbo_media_jni.c
    ${TURBO_MEDIA_ANDROID_ROOT}/src/codec/hardware_codec_android.c
    ${TURBO_MEDIA_ANDROID_ROOT}/src/mobile/mobile_optimizations.c
    ${TURBO_MEDIA_ANDROID_ROOT}/src/mobile/battery_monitor.c
    ${TURBO_MEDIA_ANDROID_ROOT}/src/mobile/network_monitor.c)

set(TURBO_MEDIA_ANDROID_JAVA_SOURCES
    ${TURBO_MEDIA_ANDROID_ROOT}/java/com/turbonet/media/MobileOptimizer.java
    ${TURBO_MEDIA_ANDROID_ROOT}/java/com/turbonet/media/ScreenCapture.java)

set(TURBO_MEDIA_ANDROID_STAGING_SOURCES
    ${TURBO_MEDIA_ANDROID_NATIVE_SOURCES}
    ${TURBO_MEDIA_ANDROID_JAVA_SOURCES})

if(ANDROID)
  project(TurboMediaAndroid LANGUAGES C)

  find_path(MINIAUDIO_INCLUDE_DIRS NAMES miniaudio.h REQUIRED)
  find_library(log-lib log REQUIRED)
  find_library(android-lib android REQUIRED)
  find_library(OpenSLES-lib OpenSLES REQUIRED)
  find_library(mediandk-lib mediandk REQUIRED)
  find_library(camera2ndk-lib camera2ndk REQUIRED)
  find_library(jnigraphics-lib jnigraphics REQUIRED)

  add_library(
    turbo_media_android
    SHARED
    ${TURBO_MEDIA_ANDROID_NATIVE_SOURCES})
  add_library(TurboMedia::Android ALIAS turbo_media_android)

  target_include_directories(
    turbo_media_android
    PUBLIC $<BUILD_INTERFACE:${TURBO_MEDIA_MOBILE_ROOT}>
           $<BUILD_INTERFACE:${TURBO_MEDIA_ROOT}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${TURBO_MEDIA_ANDROID_ROOT}/src
            ${TURBO_MEDIA_ROOT}/playback
            ${MINIAUDIO_INCLUDE_DIRS})

  target_compile_definitions(turbo_media_android PRIVATE TURBO_MEDIA_ANDROID=1 SHARED_CXX)
  target_compile_options(
    turbo_media_android
    PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)

  target_link_libraries(
    turbo_media_android
    PUBLIC TurboMedia::Device
    PRIVATE ${log-lib}
            ${android-lib}
            ${OpenSLES-lib}
            ${mediandk-lib}
            ${camera2ndk-lib}
            ${jnigraphics-lib})

  set_target_properties(
    turbo_media_android
    PROPERTIES FOLDER "turbomedia/mobile"
               EXPORT_NAME Android
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}")
else()
  project(TurboMediaAndroidStaging NONE)

  add_custom_target(turbo_media_android_staging SOURCES ${TURBO_MEDIA_ANDROID_STAGING_SOURCES})
  set_target_properties(turbo_media_android_staging PROPERTIES FOLDER "turbomedia/mobile")

  message(STATUS "TurboMedia Android sources are staged only on non-Android toolchains.")
endif()
