function(turbo_media_add_webrtc_test name)
  add_executable(${name} ${ARGN})
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
  add_test(NAME ${name} COMMAND ${name})
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
                      PRIVATE TurboNet::CoroNet OpenSSL::Crypto)

turbo_media_add_webrtc_test(turbo_media_test_signaling_lifecycle
                            test_signaling_lifecycle.c)
target_link_libraries(turbo_media_test_signaling_lifecycle
                      PRIVATE TurboMedia::WebRTCSignaling)
