cmake_minimum_required(VERSION 3.20)

project(TurboMediaPipelinePackageConsumer LANGUAGES C)

find_package(TurboMedia 1.0 CONFIG REQUIRED COMPONENTS Pipeline RTSP Transport WebRTC)

add_executable(turbo_media_pipeline_package_consumer main.c)
target_link_libraries(
  turbo_media_pipeline_package_consumer PRIVATE TurboMedia::Pipeline
                                                TurboMedia::RTSP
                                                TurboMedia::Transport
                                                TurboMedia::WebRTC)
