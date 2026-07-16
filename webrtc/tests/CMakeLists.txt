function(turbo_media_add_webrtc_test name)
  if(ANDROID AND NOT BUILD_TESTING)
    add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
  else()
    add_executable(${name} ${ARGN})
  endif()
  target_include_directories(
    ${name}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${Stb_INCLUDE_DIR})
  target_link_libraries(
    ${name}
    PRIVATE TurboMedia::DataChannel
            TurboMedia::SDP
            TurboUtils::TinyTest
            TurboUtils::Core
            TurboUtils::Parser)
  if(NOT ANDROID OR BUILD_TESTING)
    add_test(NAME ${name} COMMAND ${name})
  endif()
  set_target_properties(
    ${name}
    PROPERTIES FOLDER "turbomedia/tests/webrtc"
               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endfunction()

turbo_media_add_webrtc_test(turbo_media_test_sdp_parser test_sdp_parser.c)
turbo_media_add_webrtc_test(turbo_media_test_datachannel test_datachannel.c)
turbo_media_add_webrtc_test(turbo_media_test_dc_msg test_dc_msg.c)

turbo_media_add_webrtc_test(turbo_media_test_ice_integration
                            test_ice_integration.c)
target_link_libraries(turbo_media_test_ice_integration
                      PRIVATE TurboMedia::WebRTC TurboNet::Ice)

turbo_media_add_webrtc_test(turbo_media_test_webrtc_e2e
                            test_e2e_connection.c)
target_link_libraries(turbo_media_test_webrtc_e2e
                      PRIVATE TurboMedia::WebRTC TurboNet::Ice)

turbo_media_add_webrtc_test(turbo_media_test_signaling_json
                            test_signaling_json.c)

turbo_media_add_webrtc_test(turbo_media_test_signaling_internals
                            test_signaling_internals.c)
target_link_libraries(turbo_media_test_signaling_internals
                      PRIVATE TurboNet::CoroNet)

turbo_media_add_webrtc_test(turbo_media_test_signaling_lifecycle
                            test_signaling_lifecycle.c)
target_link_libraries(turbo_media_test_signaling_lifecycle
                      PRIVATE TurboMedia::WebRTCSignaling)

function(turbo_media_add_rtc_test name source)
  turbo_media_add_webrtc_test(${name} ${source})
  target_include_directories(
    ${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/rtc/media)
  target_link_libraries(${name} PRIVATE TurboMedia::RTC)
  set_target_properties(${name} PROPERTIES FOLDER "turbomedia/tests/rtc")
endfunction()

turbo_media_add_rtc_test(turbo_media_test_rtp test_rtp.c)
turbo_media_add_rtc_test(turbo_media_test_srtp test_srtp.c)
turbo_media_add_rtc_test(turbo_media_test_jitter_buffer test_jitter_buffer.c)
turbo_media_add_rtc_test(turbo_media_test_twcc test_twcc.c)
turbo_media_add_rtc_test(turbo_media_test_nack test_nack.c)
turbo_media_add_rtc_test(turbo_media_test_media_engine test_media_engine.c)
if(TURBO_MEDIA_ENABLE_VPX)
  turbo_media_add_rtc_test(turbo_media_test_simulcast test_simulcast.c)
endif()
