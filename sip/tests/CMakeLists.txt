set(sip_test_libraries Salts::TinyTest TurboMedia::SIP TurboMedia::RTP TurboMedia::SDP)
set(sip_test_includes
  ${CMAKE_CURRENT_SOURCE_DIR}/../signaling
  ${CMAKE_CURRENT_SOURCE_DIR}/../signaling/uac)

cmake_add_test(turbo_media_test_sip_uac
  SOURCES test_sip_uac.c
  LIBS ${sip_test_libraries}
  INCLUDES ${sip_test_includes}
  FOLDER "turbomedia/tests/sip")

if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  cmake_add_test(turbo_media_test_sip_uac_e2e
    SOURCES test_sip_uac_e2e.c
    LIBS ${sip_test_libraries} Salts::CNet Salts::Crypto TurboMedia::AudioCodec TurboMedia::RTPICE
    INCLUDES ${sip_test_includes}
    FOLDER "turbomedia/tests/sip")
  set_tests_properties(turbo_media_test_sip_uac_e2e PROPERTIES TIMEOUT 35)
endif()

cmake_add_test(turbo_media_test_sip_client
  SOURCES test_sip_client.c
  LIBS Salts::TinyTest TurboMedia::SIPClient
  FOLDER "turbomedia/tests/sip")
set_tests_properties(turbo_media_test_sip_client PROPERTIES TIMEOUT 15)
