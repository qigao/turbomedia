find_package(unofficial-usrsctp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(roaring CONFIG REQUIRED)

find_program(TURBO_MEDIA_RE2C_EXECUTABLE re2c REQUIRED)
set(TURBO_MEDIA_SDP_PARSER_RE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp/turbo_sdp_parser.re)
set(TURBO_MEDIA_SDP_PARSER_GEN
    ${CMAKE_CURRENT_BINARY_DIR}/turbo_sdp_parser_gen.c)
add_custom_command(
  OUTPUT ${TURBO_MEDIA_SDP_PARSER_GEN}
  COMMAND ${TURBO_MEDIA_RE2C_EXECUTABLE} -o ${TURBO_MEDIA_SDP_PARSER_GEN}
          ${TURBO_MEDIA_SDP_PARSER_RE}
  DEPENDS ${TURBO_MEDIA_SDP_PARSER_RE}
  COMMENT "Generating WebRTC SDP parser"
  VERBATIM)

add_library(turbo_media_sdp STATIC)
turbo_media_configure_component(turbo_media_sdp SDP)
turbo_media_add_component_alias(turbo_media_sdp TurboMedia::SDP)
target_sources(
  turbo_media_sdp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp/turbo_sdp.c
          ${CMAKE_CURRENT_SOURCE_DIR}/src/stb_sprintf.c
          ${TURBO_MEDIA_SDP_PARSER_GEN})
target_include_directories(
  turbo_media_sdp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp
          ${CMAKE_CURRENT_BINARY_DIR}
          ${Stb_INCLUDE_DIR})
target_link_libraries(turbo_media_sdp PRIVATE TurboUtils::Core)

set(TURBO_MEDIA_DATACHANNEL_SOURCES
    src/turbo_datachannel.c
    src/turbo_datachannel_errors.c
    src/turbo_dc_msg.c
    src/dcep_protocol.c
    src/dtls_session.c
    src/sctp_session.c
    src/stb_sprintf.c)

add_library(turbo_media_datachannel SHARED)
turbo_media_configure_component(turbo_media_datachannel DataChannel)
turbo_media_add_component_alias(turbo_media_datachannel TurboMedia::DataChannel)
target_sources(turbo_media_datachannel PRIVATE ${TURBO_MEDIA_DATACHANNEL_SOURCES})
target_include_directories(
  turbo_media_datachannel
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
  turbo_media_datachannel
  PUBLIC TurboMedia::SDP
  PRIVATE TurboNet::CoroNet
          TurboNet::Ice
          TurboUtils::Core
          TurboUtils::Parser
          unofficial::usrsctp::usrsctp
          OpenSSL::SSL
          OpenSSL::Crypto
          TurboNet::Asn1Parser
          roaring::roaring)

add_library(turbo_media_webrtc SHARED)
turbo_media_configure_component(turbo_media_webrtc WebRTC)
turbo_media_add_component_alias(turbo_media_webrtc TurboMedia::WebRTC)
target_sources(
  turbo_media_webrtc
  PRIVATE adapter/webrtc_adapter.c
          adapter/webrtc_session.c
          src/ice_integration.c)
target_include_directories(
  turbo_media_webrtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(
  turbo_media_webrtc
  PUBLIC TurboMedia::Core TurboMedia::Server TurboMedia::DataChannel TurboNet::Ice)
target_compile_definitions(turbo_media_webrtc PUBLIC TURBO_MEDIA_HAS_WEBRTC)

add_library(turbo_media_webrtc_signaling SHARED)
turbo_media_configure_component(turbo_media_webrtc_signaling WebRTCSignaling)
turbo_media_add_component_alias(turbo_media_webrtc_signaling
                                TurboMedia::WebRTCSignaling)
target_sources(
  turbo_media_webrtc_signaling
  PRIVATE src/signaling/webrtc_signaling.c
          src/signaling/http_api.c)
target_include_directories(
  turbo_media_webrtc_signaling
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${TURBO_MEDIA_IRIS_INCLUDE_DIR}
          ${Stb_INCLUDE_DIR})
target_link_libraries(
  turbo_media_webrtc_signaling
  PUBLIC TurboNet::CoroNet TurboHttp::Iris
  PRIVATE TurboUtils::Core
          TurboUtils::Parser
          OpenSSL::Crypto)
target_compile_definitions(turbo_media_webrtc_signaling
                           PUBLIC TURBO_MEDIA_HAS_WEBRTC_SIGNALING)

add_library(turbo_media_rtc INTERFACE)
turbo_media_add_component_alias(turbo_media_rtc TurboMedia::RTC)
set_target_properties(turbo_media_rtc PROPERTIES EXPORT_NAME RTC)
target_link_libraries(
  turbo_media_rtc
  INTERFACE TurboMedia::WebRTC TurboMedia::DataChannel TurboMedia::SDP)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
