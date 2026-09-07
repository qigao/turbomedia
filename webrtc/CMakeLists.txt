cmake_add_grammar(
  TURBO_MEDIA_SDP
  LEXER_RE src/sdp/turbo_sdp_parser.re
  LEXER_OUTPUT turbo_sdp_parser_gen.c
  FOLDER "turbomedia/webrtc/codegen")

add_library(turbo_media_auth STATIC src/auth/turbo_media_auth.c)
target_include_directories(
  turbo_media_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
target_link_libraries(
  turbo_media_auth
  PRIVATE Salts::JsonParser SaltsNet::Crypto OpenSSL::Crypto)
set_target_properties(turbo_media_auth PROPERTIES FOLDER "turbomedia/components")

add_library(turbo_media_sdp STATIC)
turbo_media_configure_component(turbo_media_sdp SDP)
target_sources(
  turbo_media_sdp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp/turbo_sdp.c
          ${CMAKE_CURRENT_SOURCE_DIR}/src/stb_sprintf.c
          ${TURBO_MEDIA_SDP_LEXER_GEN})
target_include_directories(
  turbo_media_sdp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp
          ${CMAKE_CURRENT_BINARY_DIR}
          ${STB_INCLUDE_DIR})
target_link_libraries(turbo_media_sdp PRIVATE Salts::Core)

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
target_sources(turbo_media_datachannel PRIVATE ${TURBO_MEDIA_DATACHANNEL_SOURCES})
target_include_directories(
  turbo_media_datachannel
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
  turbo_media_datachannel
  PUBLIC TurboMedia::SDP Salts::LtvParser
  PRIVATE Salts::CNet
          SaltsNet::ICE
          Salts::Core
          Salts::CSTL
          unofficial::usrsctp::usrsctp
          OpenSSL::SSL
          OpenSSL::Crypto
          SaltsNet::Asn1Parser
          roaring::roaring)

add_library(turbo_media_webrtc SHARED)
turbo_media_configure_component(turbo_media_webrtc WebRTC)
target_sources(
  turbo_media_webrtc
  PRIVATE adapter/webrtc_adapter.c
          adapter/turbo_peer_backend.c
          adapter/webrtc_session.c
          src/salts_ice_owner.c
          src/ice_integration.c)
target_include_directories(
  turbo_media_webrtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
  turbo_media_webrtc
  PUBLIC TurboMedia::Core TurboMedia::Server TurboMedia::DataChannel SaltsNet::ICE
  PRIVATE TurboMedia::RTC TurboMedia::SDP Salts::Core)
target_compile_definitions(turbo_media_webrtc PUBLIC TURBO_MEDIA_HAS_WEBRTC)

add_library(turbo_media_webrtc_signaling SHARED)
turbo_media_configure_component(turbo_media_webrtc_signaling WebRTCSignaling)
target_sources(
  turbo_media_webrtc_signaling
  PRIVATE src/signaling/webrtc_signaling.c
          src/signaling/http_api.c)
target_include_directories(
  turbo_media_webrtc_signaling
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${STB_INCLUDE_DIR})
target_link_libraries(
  turbo_media_webrtc_signaling
  PUBLIC Salts::CNet Salts::CHTTP
  PRIVATE turbo_media_auth Salts::JsonParser SaltsNet::Crypto Salts::Core Salts::CSTL)
target_compile_definitions(turbo_media_webrtc_signaling
                           PUBLIC TURBO_MEDIA_HAS_WEBRTC_SIGNALING)

set(TURBO_MEDIA_RTC_SOURCES
    src/salts_ice_owner.c
    src/rtc/rtp/rtp_packet.c
    src/rtc/rtp/rtcp_packet.c
    src/rtc/rtp/rtp_session.c
    src/rtc/rtp/rtp_history.c
    src/rtc/rtp/twcc.c
    src/rtc/rtp/simulcast.c
    src/rtc/rtp/nack.c
    src/rtc/srtp/srtp_session.c
    src/rtc/media/jitter_buffer.c
    src/rtc/media/turbo_media_engine.c
    src/rtc/media/turbo_peer_connection.c)

add_library(turbo_media_rtc SHARED)
turbo_media_configure_component(turbo_media_rtc RTC)
target_sources(turbo_media_rtc PRIVATE ${TURBO_MEDIA_RTC_SOURCES})
target_include_directories(
  turbo_media_rtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/src/rtc/media)
target_link_libraries(
  turbo_media_rtc
  PUBLIC TurboMedia::DataChannel
         TurboMedia::SDP
         TurboMedia::Crypto
         TurboMedia::Speech
         TurboMedia::Recognition
  PRIVATE TurboMedia::Codec
          Salts::Capture
          SaltsNet::ICE
          Salts::CNet
          Salts::Core
          Salts::CSTL
          OpenSSL::SSL
          OpenSSL::Crypto
          libSRTP::srtp2)
target_compile_definitions(turbo_media_rtc PUBLIC TURBO_MEDIA_HAS_RTC)
# See transport/CMakeLists.txt: RTC and legacy transport packets have distinct
# layouts, so internal calls must never be preempted by the peer shared object.
target_link_options(
  turbo_media_rtc
  PRIVATE "$<$<PLATFORM_ID:Linux>:LINKER:-Bsymbolic-functions>")

# Room-service / SFU server apps are desktop targets; keep them out of the
# Android build (they were gated with NOT ANDROID before the RTC-apps split).
if(NOT ANDROID)
  # IVR media worker first so RoomService can consume its typed H1 WebSocket bridge.
  # The server applications remain desktop-only.
  add_subdirectory(ivr)
  add_subdirectory(apps)
endif()
add_subdirectory(tests)
add_subdirectory(examples)
