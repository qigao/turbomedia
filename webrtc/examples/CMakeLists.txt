# WebRTC DataChannel Examples

cmake_minimum_required(VERSION 3.20)

# Simple echo example
add_executable(dc_simple_echo simple_echo.c)
target_link_libraries(dc_simple_echo PRIVATE TurboMedia::DataChannel TurboUtils::Core)
set_target_properties(dc_simple_echo PROPERTIES FOLDER webrtc/examples)

# File transfer example
add_executable(dc_file_transfer file_transfer.c)
target_link_libraries(
  dc_file_transfer PRIVATE TurboMedia::DataChannel TurboUtils::Core)
set_target_properties(dc_file_transfer PROPERTIES FOLDER webrtc/examples)

# Benchmark
add_executable(dc_benchmark benchmark.c)
target_link_libraries(dc_benchmark PRIVATE TurboMedia::DataChannel TurboUtils::Core)
set_target_properties(dc_benchmark PROPERTIES FOLDER webrtc/examples)

add_executable(rtc_signaled_peer signaled_peer.c)
target_link_libraries(rtc_signaled_peer PRIVATE TurboMedia::DataChannel TurboMedia::SDP
                                               TurboParser::Parser TurboNet::CoroNet TurboNet::Ice
                                               TurboUtils::Core
                                               $<$<PLATFORM_ID:Windows>:ws2_32>)
set_target_properties(rtc_signaled_peer PROPERTIES FOLDER webrtc/examples)

add_executable(dc_ice_example ice_datachannel.c)
target_link_libraries(
  dc_ice_example PRIVATE TurboMedia::DataChannel TurboNet::Ice TurboUtils::Core)
target_include_directories(dc_ice_example PRIVATE ${STB_INCLUDE_DIR})
set_target_properties(dc_ice_example PROPERTIES FOLDER webrtc/examples)

add_executable(browser_interop browser_interop.c)
target_link_libraries(browser_interop PRIVATE TurboMedia::WebRTC TurboUtils::Core)
set_target_properties(browser_interop PROPERTIES FOLDER webrtc/examples)

add_executable(browser_media_interop browser_media_interop.c)
target_link_libraries(
  browser_media_interop
  PRIVATE TurboMedia::RTC TurboMedia::WebRTC TurboNet::Ice TurboUtils::Core)
set_target_properties(browser_media_interop PROPERTIES FOLDER webrtc/examples)

# =============================================================================
# Signaling Server Example
# =============================================================================

add_executable(rtc_server signaling_server.c)
target_link_libraries(rtc_server PRIVATE TurboMedia::WebRTCSignaling TurboMedia::SDP)
set_target_properties(rtc_server PROPERTIES FOLDER webrtc/examples)
