cmake_minimum_required(VERSION 3.20)

project(TurboMediaPackageConsumers LANGUAGES C)

find_package(TurboMedia 2.0 CONFIG REQUIRED
             COMPONENTS Pipeline RTSP Transport WebRTC DataChannel Player Speech Recognition)

add_executable(turbo_media_pipeline_package_consumer main.c)
target_link_libraries(turbo_media_pipeline_package_consumer
                      PRIVATE TurboMedia::Pipeline)

add_executable(turbo_media_rtsp_package_consumer rtsp_consumer.c)
target_link_libraries(turbo_media_rtsp_package_consumer PRIVATE TurboMedia::RTSP)

add_executable(turbo_media_transport_package_consumer transport_consumer.c)
target_link_libraries(turbo_media_transport_package_consumer
                      PRIVATE TurboMedia::Transport)

add_executable(turbo_media_webrtc_package_consumer webrtc_consumer.c)
target_link_libraries(turbo_media_webrtc_package_consumer
                      PRIVATE TurboMedia::WebRTC)

add_executable(turbo_media_datachannel_package_consumer datachannel_consumer.c)
target_link_libraries(turbo_media_datachannel_package_consumer
                      PRIVATE TurboMedia::DataChannel)

add_executable(turbo_media_player_package_consumer player_consumer.c)
target_link_libraries(turbo_media_player_package_consumer
                      PRIVATE TurboMedia::Player)

add_executable(turbo_media_speech_package_consumer speech_consumer.c)
target_link_libraries(turbo_media_speech_package_consumer
                      PRIVATE TurboMedia::Speech)

add_executable(turbo_media_recognition_package_consumer recognition_consumer.c)
target_link_libraries(turbo_media_recognition_package_consumer
                      PRIVATE TurboMedia::Recognition)
