# Room-service / SFU model layer backing the apps below.
# Imported from turbo-webrtc media (conference/sfu/recording) so the apps no
# longer depend on the external TurboRTCApps package.
add_library(
  turbo_media_rtc_apps
  STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/conference/turbo_room_service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/conference/turbo_sfu_node.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/sfu/turbo_sfu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/recording/turbo_recorder.c)
turbo_media_configure_component(turbo_media_rtc_apps RtcApps)
target_include_directories(
  turbo_media_rtc_apps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/recording)
target_link_libraries(
  turbo_media_rtc_apps
  PUBLIC TurboMedia::RTC TurboUtils::Core
  PRIVATE TurboMedia::Muxer TurboMedia::Transport TurboNet::Crypto)

add_subdirectory(room_service)
add_subdirectory(sfu_node)
if(TURBO_MEDIA_HAS_IVR_FMQ)
  add_subdirectory(ivr_worker)
endif()
add_subdirectory(signaling_server)
