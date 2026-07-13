cmake_minimum_required(VERSION 3.20)

set(TURBO_MEDIA_MOBILE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(TURBO_MEDIA_IOS_ROOT ${CMAKE_CURRENT_LIST_DIR})

set(TURBO_MEDIA_IOS_NATIVE_SOURCES
    ${TURBO_MEDIA_MOBILE_ROOT}/turbo_mobile_codec.h
    ${TURBO_MEDIA_MOBILE_ROOT}/turbo_mobile.h
    ${TURBO_MEDIA_IOS_ROOT}/src/capture/capture_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/capture/capture_video_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/capture/capture_audio_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/capture/capture_screen_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/codec/hardware_codec_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/mobile/mobile_optimizations_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/mobile/battery_monitor_ios.m
    ${TURBO_MEDIA_IOS_ROOT}/src/mobile/network_monitor_ios.m)

set(TURBO_MEDIA_IOS_WRAPPER_SOURCES ${TURBO_MEDIA_IOS_ROOT}/swift/TurboMedia.swift)

set(TURBO_MEDIA_IOS_STAGING_SOURCES
    ${TURBO_MEDIA_IOS_NATIVE_SOURCES}
    ${TURBO_MEDIA_IOS_WRAPPER_SOURCES})

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  project(TurboMediaIOS LANGUAGES C OBJC)

  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum iOS deployment version")

  add_library(turbo_media_ios STATIC ${TURBO_MEDIA_IOS_NATIVE_SOURCES})

  target_include_directories(
    turbo_media_ios
    PUBLIC ${TURBO_MEDIA_MOBILE_ROOT}
    PRIVATE ${TURBO_MEDIA_IOS_ROOT}/src)

  target_compile_definitions(turbo_media_ios PRIVATE TURBO_MEDIA_IOS=1)
  target_compile_options(
    turbo_media_ios
    PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fobjc-arc)

  target_link_libraries(
    turbo_media_ios
    PRIVATE "-framework Foundation"
            "-framework AVFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework ReplayKit"
            "-framework UIKit"
            "-framework Network")

  set_target_properties(
    turbo_media_ios
    PROPERTIES FOLDER "turbomedia/mobile"
               XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
               XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
else()
  project(TurboMediaIOSStaging NONE)

  add_custom_target(turbo_media_ios_staging SOURCES ${TURBO_MEDIA_IOS_STAGING_SOURCES})
  set_target_properties(turbo_media_ios_staging PROPERTIES FOLDER "turbomedia/mobile")

  message(STATUS "TurboMedia iOS sources are staged only on non-iOS toolchains.")
endif()
