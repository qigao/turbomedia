cmake_add_grammar(
  TURBO_MEDIA_SDP
  LEXER_RE src/sdp/turbo_sdp_parser.re
  LEXER_OUTPUT turbo_sdp_parser_gen.c
  FOLDER "turbomedia/webrtc/codegen")

add_library(
  turbo_media_http_media_client STATIC
  src/http/turbo_http_media_client.c)
target_include_directories(
  turbo_media_http_media_client
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/http)
target_link_libraries(
  turbo_media_http_media_client
  PRIVATE CHttp::Client Salts::CNet Salts::Core)
set_target_properties(
  turbo_media_http_media_client
  PROPERTIES FOLDER "turbomedia/components")

add_library(
  turbo_media_auth STATIC
  src/auth/turbo_media_auth.c
  src/auth/turbo_media_revocation.c
  src/auth/turbo_media_revocation_projection.c
  src/auth/turbo_media_revocation_wire.c
  src/auth/turbo_media_revocation_fanout.c)
target_include_directories(
  turbo_media_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
target_link_libraries(
  turbo_media_auth
  PRIVATE Salts::JsonParser OpenSSL::Crypto Salts::Core)
set_target_properties(turbo_media_auth PROPERTIES FOLDER "turbomedia/components")

add_library(
  turbo_media_tenant_quota STATIC
  src/security/turbo_media_tenant_quota.c
  src/security/turbo_media_tenant_quota_allocator.c)
target_include_directories(
  turbo_media_tenant_quota PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/security)
set_target_properties(
  turbo_media_tenant_quota PROPERTIES FOLDER "turbomedia/components")

add_library(
  turbo_media_tenant_quota_wire STATIC
  src/security/turbo_media_tenant_quota_wire.c)
target_include_directories(
  turbo_media_tenant_quota_wire
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/security)
target_link_libraries(
  turbo_media_tenant_quota_wire
  PRIVATE turbo_media_tenant_quota Salts::JsonParser)
set_target_properties(
  turbo_media_tenant_quota_wire PROPERTIES FOLDER "turbomedia/components")

add_library(
  turbo_media_revocation_https STATIC
  src/auth/turbo_media_revocation_https.c)
target_include_directories(
  turbo_media_revocation_https PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/auth)
target_link_libraries(
  turbo_media_revocation_https
  PRIVATE turbo_media_auth TurboMedia::Transport Salts::JsonParser)
set_target_properties(
  turbo_media_revocation_https PROPERTIES FOLDER "turbomedia/components")

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
    src/dc_certificate.cpp
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
  PRIVATE turbo_media_cnet_adapter
          Salts::CNet
          SaltsNet::ICE
          Salts::Core
          Salts::CSTL
          unofficial::usrsctp::usrsctp
          OpenSSL::SSL
          OpenSSL::Crypto
          roaring::roaring)

if(TURBO_MEDIA_HAS_DESKTOP_SERVICES)
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
endif()

add_library(turbo_media_webrtc_signaling SHARED)
turbo_media_configure_component(turbo_media_webrtc_signaling WebRTCSignaling)
target_sources(
  turbo_media_webrtc_signaling
  PRIVATE src/signaling/webrtc_signaling.c
          src/signaling/signaling_source_identity.c
          src/signaling/http_api.c)
target_include_directories(
  turbo_media_webrtc_signaling
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${STB_INCLUDE_DIR})
target_link_libraries(
  turbo_media_webrtc_signaling
  PUBLIC Salts::CNet CHttp::Server
  PRIVATE turbo_media_auth turbo_media_tenant_quota
          turbo_media_tenant_quota_wire
          Salts::JsonParser Salts::Core Salts::CSTL)
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
          SaltsNet::ICE
          Salts::CNet
          Salts::Core
          Salts::CSTL
          OpenSSL::SSL
          OpenSSL::Crypto
          libSRTP::srtp2)
target_compile_definitions(turbo_media_rtc PUBLIC TURBO_MEDIA_HAS_RTC)
target_link_libraries(turbo_media_rtc PRIVATE Salts::Capture)
# See transport/CMakeLists.txt: RTC and legacy transport packets have distinct
# layouts, so internal calls must never be preempted by the peer shared object.
target_link_options(
  turbo_media_rtc
  PRIVATE "$<$<PLATFORM_ID:Linux>:LINKER:-Bsymbolic-functions>")

add_library(turbo_media_rtc_client SHARED
    src/client/turbo_rtc_client.c
    src/client/turbo_rtc_subscriber.c
    src/client/turbo_rtc_playback_sink.c
    src/client/turbo_rtc_capture_source.c)
  turbo_media_configure_component(turbo_media_rtc_client RTCClient)
  target_include_directories(
    turbo_media_rtc_client
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/http)
  target_link_libraries(
    turbo_media_rtc_client
    PUBLIC Salts::Playback
    PRIVATE TurboMedia::RTC
            turbo_media_http_media_client
            Salts::Capture
            Salts::Platform
            Salts::Core)

# Room-service / SFU server apps are desktop targets; keep them out of the
# Android build (they were gated with NOT ANDROID before the RTC-apps split).
if(TURBO_MEDIA_HAS_DESKTOP_SERVICES)
  # IVR media worker first so RoomService can consume its typed H1 WebSocket bridge.
  # The server applications remain desktop-only.
  add_subdirectory(ivr)
  add_subdirectory(apps)
endif()
add_subdirectory(tests)
add_subdirectory(examples)
