function(turbo_media_add_webrtc_base_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(
    ${name}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${STB_INCLUDE_DIR})
  target_link_libraries(
    ${name}
    PRIVATE Salts::TinyTest
            Salts::Core
            Salts::CSTL)
  if(NOT ANDROID)
    add_test(NAME ${name} COMMAND ${name})
  endif()
  set_target_properties(
    ${name}
    PROPERTIES FOLDER "turbomedia/tests/webrtc"
               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endfunction()

function(turbo_media_add_webrtc_test name)
  turbo_media_add_webrtc_base_test(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE TurboMedia::DataChannel TurboMedia::SDP)
endfunction()

turbo_media_add_webrtc_base_test(
  turbo_media_test_salts_ice_owner
  test_salts_ice_owner.c
  ../src/salts_ice_owner.c)
target_include_directories(
  turbo_media_test_salts_ice_owner
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(
  turbo_media_test_salts_ice_owner PRIVATE SaltsNet::ICE)

turbo_media_add_webrtc_test(turbo_media_test_sdp_parser test_sdp_parser.c)
turbo_media_add_webrtc_test(turbo_media_test_datachannel test_datachannel.c)
turbo_media_add_webrtc_test(turbo_media_test_dc_msg test_dc_msg.c)
target_link_libraries(turbo_media_test_dc_msg PRIVATE Salts::LtvParser)

if(TURBO_MEDIA_IS_SERVER)
  turbo_media_add_webrtc_test(turbo_media_test_ice_integration
                              test_ice_integration.c)
  target_link_libraries(turbo_media_test_ice_integration
                        PRIVATE TurboMedia::WebRTC SaltsNet::ICE)

  turbo_media_add_webrtc_test(turbo_media_test_webrtc_e2e
                              test_e2e_connection.c)
  target_link_libraries(turbo_media_test_webrtc_e2e
                        PRIVATE TurboMedia::WebRTC TurboMedia::RTC SaltsNet::ICE)
endif()

turbo_media_add_webrtc_test(turbo_media_test_signaling_json
                            test_signaling_json.c)
target_link_libraries(turbo_media_test_signaling_json PRIVATE Salts::JsonParser)

turbo_media_add_webrtc_base_test(turbo_media_test_auth_token test_auth_token.c)
target_link_libraries(turbo_media_test_auth_token
                      PRIVATE turbo_media_auth SaltsNet::Crypto)

turbo_media_add_webrtc_base_test(turbo_media_test_revocation_state
                                 test_revocation_state.c)
target_link_libraries(turbo_media_test_revocation_state
                      PRIVATE turbo_media_auth SaltsNet::Crypto)

turbo_media_add_webrtc_base_test(turbo_media_test_revocation_projection
                                 test_revocation_projection.c)
target_link_libraries(turbo_media_test_revocation_projection
                      PRIVATE turbo_media_auth SaltsNet::Crypto)

turbo_media_add_webrtc_base_test(turbo_media_test_revocation_fanout
                                 test_revocation_fanout.c)
target_link_libraries(turbo_media_test_revocation_fanout
                      PRIVATE turbo_media_auth)

turbo_media_add_webrtc_base_test(turbo_media_test_revocation_https
                                 test_revocation_https.c)
target_link_libraries(turbo_media_test_revocation_https
                      PRIVATE turbo_media_revocation_https)

turbo_media_add_webrtc_test(turbo_media_test_signaling_internals
                            test_signaling_internals.c)
target_link_libraries(turbo_media_test_signaling_internals
                      PRIVATE Salts::CNet Salts::CHTTP turbo_media_auth
                              Salts::JsonParser)

turbo_media_add_webrtc_test(turbo_media_test_signaling_lifecycle
                            test_signaling_lifecycle.c)
target_link_libraries(turbo_media_test_signaling_lifecycle
                      PRIVATE TurboMedia::WebRTCSignaling TurboMedia::Transport
                              Salts::CHTTP
                              turbo_media_auth)
file(TO_CMAKE_PATH
     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tls/localhost-cert.pem"
     TURBO_MEDIA_TEST_TLS_CERT_PATH)
file(TO_CMAKE_PATH
     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tls/localhost-key.pem"
     TURBO_MEDIA_TEST_TLS_KEY_PATH)
target_compile_definitions(
  turbo_media_test_signaling_lifecycle
  PRIVATE TURBO_MEDIA_TEST_TLS_CERT_PATH="${TURBO_MEDIA_TEST_TLS_CERT_PATH}"
          TURBO_MEDIA_TEST_TLS_KEY_PATH="${TURBO_MEDIA_TEST_TLS_KEY_PATH}")

if(TURBO_MEDIA_IS_SERVER)
  turbo_media_add_webrtc_test(
    turbo_media_test_signaling_config
    test_signaling_config.c
    ../apps/signaling_server/src/config.c)
  target_include_directories(
    turbo_media_test_signaling_config
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apps/signaling_server/include)
  target_link_libraries(turbo_media_test_signaling_config
                        PRIVATE turbo_media_auth Salts::TomlParser)
  file(
    TO_CMAKE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../apps/signaling_server/config/signaling.toml.example"
    SIGNALING_CONFIG_EXAMPLE_PATH)
  target_compile_definitions(
    turbo_media_test_signaling_config
    PRIVATE SIGNALING_CONFIG_EXAMPLE_PATH="${SIGNALING_CONFIG_EXAMPLE_PATH}")
endif()

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
turbo_media_add_rtc_test(turbo_media_test_speech_integration
                         test_speech_integration.c)
turbo_media_add_rtc_test(turbo_media_test_recognition_integration
                         test_recognition_integration.c)
turbo_media_add_rtc_test(turbo_media_test_simulcast test_simulcast.c)
target_link_libraries(turbo_media_test_simulcast PRIVATE TurboMedia::Codec)
