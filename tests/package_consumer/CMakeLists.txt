cmake_minimum_required(VERSION 3.20)

project(TurboMediaPackageConsumers LANGUAGES C)

if(NOT TurboMedia_EXPECTED_PRODUCT MATCHES "^(CLIENT|SERVER)$")
  message(FATAL_ERROR "TurboMedia_EXPECTED_PRODUCT must be CLIENT or SERVER")
endif()

set(turbo_media_components RTSP Transport DataChannel Speech Recognition)
if(TurboMedia_EXPECTED_PRODUCT STREQUAL "CLIENT")
  list(APPEND turbo_media_components Player)
else()
  list(APPEND turbo_media_components Pipeline WebRTC)
endif()
find_package(TurboMedia 2.0 CONFIG REQUIRED
             COMPONENTS ${turbo_media_components})
if(NOT TurboMedia_PRODUCT STREQUAL TurboMedia_EXPECTED_PRODUCT)
  message(FATAL_ERROR
          "Expected ${TurboMedia_EXPECTED_PRODUCT}, found ${TurboMedia_PRODUCT}")
endif()

if(TurboMedia_EXPECTED_PRODUCT STREQUAL "SERVER")
  if(TARGET TurboMedia::Player)
    message(FATAL_ERROR "SERVER package unexpectedly exports Player")
  endif()
  add_executable(turbo_media_pipeline_package_consumer main.c)
  target_link_libraries(turbo_media_pipeline_package_consumer
                        PRIVATE TurboMedia::Pipeline)

  add_executable(turbo_media_webrtc_package_consumer webrtc_consumer.c)
  target_link_libraries(turbo_media_webrtc_package_consumer
                        PRIVATE TurboMedia::WebRTC)
else()
  if(TARGET TurboMedia::Pipeline OR TARGET TurboMedia::WebRTC)
    message(FATAL_ERROR "CLIENT package unexpectedly exports server targets")
  endif()
  add_executable(turbo_media_player_package_consumer player_consumer.c)
  target_link_libraries(turbo_media_player_package_consumer
                        PRIVATE TurboMedia::Player)
endif()

add_executable(turbo_media_rtsp_package_consumer rtsp_consumer.c)
target_link_libraries(turbo_media_rtsp_package_consumer PRIVATE TurboMedia::RTSP)

add_executable(turbo_media_transport_package_consumer transport_consumer.c)
target_link_libraries(turbo_media_transport_package_consumer
                      PRIVATE TurboMedia::Transport)

add_executable(turbo_media_datachannel_package_consumer datachannel_consumer.c)
target_link_libraries(turbo_media_datachannel_package_consumer
                      PRIVATE TurboMedia::DataChannel)

add_executable(turbo_media_speech_package_consumer speech_consumer.c)
target_link_libraries(turbo_media_speech_package_consumer
                      PRIVATE TurboMedia::Speech)

add_executable(turbo_media_recognition_package_consumer recognition_consumer.c)
target_link_libraries(turbo_media_recognition_package_consumer
                      PRIVATE TurboMedia::Recognition)
