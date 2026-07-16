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
  PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(
  turbo_media_webrtc_signaling
  PUBLIC TurboNet::CoroNet TurboHttp::Iris
  PRIVATE TurboUtils::Core
          TurboUtils::Parser)
target_compile_definitions(turbo_media_webrtc_signaling
                           PUBLIC TURBO_MEDIA_HAS_WEBRTC_SIGNALING)

set(TURBO_MEDIA_RTC_SOURCES
    src/rtc/rtp/rtp_packet.c
    src/rtc/rtp/rtcp_packet.c
    src/rtc/rtp/rtp_session.c
    src/rtc/rtp/rtp_history.c
    src/rtc/rtp/twcc.c
    src/rtc/rtp/simulcast.c
    src/rtc/rtp/nack.c
    src/rtc/srtp/srtp_session.c
    src/rtc/media/jitter_buffer.c
    src/rtc/media/turbo_media_engine.c)

add_library(turbo_media_rtc SHARED)
turbo_media_configure_component(turbo_media_rtc RTC)
turbo_media_add_component_alias(turbo_media_rtc TurboMedia::RTC)
target_sources(turbo_media_rtc PRIVATE ${TURBO_MEDIA_RTC_SOURCES})
target_include_directories(
  turbo_media_rtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rtc/media)
target_link_libraries(
  turbo_media_rtc
  PUBLIC TurboMedia::WebRTC
         TurboMedia::DataChannel
         TurboMedia::SDP
         TurboMedia::Crypto
  PRIVATE TurboMedia::Codec
          TurboMedia::Device
          TurboUtils::Core
          OpenSSL::SSL
          OpenSSL::Crypto
          libSRTP::srtp2)
target_compile_definitions(turbo_media_rtc PUBLIC TURBO_MEDIA_HAS_RTC)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
