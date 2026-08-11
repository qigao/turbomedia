set(TURBO_MEDIA_MOBILE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(TURBO_MEDIA_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_library(
  turbo_media_ios STATIC
  ${TURBO_MEDIA_ROOT}/capture/capture_video.c
  src/capture/capture_ios.m
  src/capture/capture_video_ios.m
  src/capture/capture_audio_ios.m
  src/capture/capture_screen_ios.m
  src/codec/hardware_codec_ios.m
  src/mobile/mobile_optimizations_ios.m
  src/mobile/battery_monitor_ios.m
  src/mobile/network_monitor_ios.m)
turbo_media_configure_component(turbo_media_ios IOS)
target_include_directories(
  turbo_media_ios
  PUBLIC $<BUILD_INTERFACE:${TURBO_MEDIA_MOBILE_ROOT}>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src ${TURBO_MEDIA_ROOT}/capture)
target_compile_definitions(turbo_media_ios PRIVATE TURBO_MEDIA_IOS=1)
target_compile_options(
  turbo_media_ios
  PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fobjc-arc)
target_link_libraries(
  turbo_media_ios
  PUBLIC TurboMedia::Device
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
  PROPERTIES XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
             XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
