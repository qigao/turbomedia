# TurboMedia IVR worker module (Phase 1: conference baseline).
cmake_minimum_required(VERSION 3.20)

project(
  TurboMediaIvr
  VERSION 1.0.0
  LANGUAGES C)

add_library(turbo_media_ivr STATIC
  src/ivr_acl.c
  src/ivr_certificate_identity.c
  src/ivr_dtmf_rtp.c
  src/ivr_frame.c
  src/ivr_protocol.c
  src/ivr_media_bot.c
  src/ivr_media_reconnect.c
  src/ivr_media_supervisor.c
  src/ivr_openai_provider.c
  src/ivr_thread.c
  src/ivr_util.c
  src/ivr_worker.c)
target_include_directories(
  turbo_media_ivr
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../speech/include)
target_link_libraries(
  turbo_media_ivr
  PUBLIC TurboParser::DataBind TurboParser::Parser
         TurboUtils::Core TurboMedia::Speech TurboHttp::HttpClient
  PRIVATE Threads::Threads)
set_target_properties(turbo_media_ivr PROPERTIES FOLDER "turbomedia/components")

# Real WebRTC media adapters are kept separate from the provider-neutral IVR
# core.  The RTC target is linked only by applications that opt into WHIP/WHEP.
add_library(turbo_media_ivr_rtc STATIC
  src/ivr_http_media_client.c
  src/ivr_whep_transport.c
  src/ivr_whip_transport.c)
target_include_directories(
  turbo_media_ivr_rtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}/../../speech/include)
target_link_libraries(
  turbo_media_ivr_rtc
  PUBLIC turbo_media_ivr TurboMedia::RTC
  PRIVATE TurboHttp::TurboHttp)
set_target_properties(turbo_media_ivr_rtc PROPERTIES FOLDER "turbomedia/components")

file(
  TO_CMAKE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/turbomedia_ivr_v1.schema"
  IVR_TEST_SCHEMA_PATH)

# ---------------------------------------------------------------------------
# Gate A codegen: tbe_compiler --source-output renders owning typed .h/.c from
# the canonical schema. FlowMQ messages require these generated typed bindings,
# so an IVR build fails fast when the compiler is unavailable.
# ---------------------------------------------------------------------------
turbo_media_find_tbe_compiler(TBE_COMPILER_EXECUTABLE)
set(IVR_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${IVR_GEN_DIR}")
set(IVR_GEN_HEADER "${IVR_GEN_DIR}/turbomedia_ivr_v1.h")
set(IVR_GEN_SOURCE "${IVR_GEN_DIR}/turbomedia_ivr_v1.c")
add_custom_command(
  OUTPUT ${IVR_GEN_HEADER} ${IVR_GEN_SOURCE}
  COMMAND ${TBE_COMPILER_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/schema/turbomedia_ivr_v1.schema
          --lang c --output ${IVR_GEN_HEADER} --source-output ${IVR_GEN_SOURCE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/schema/turbomedia_ivr_v1.schema
  COMMENT "Generating IVR schema typed code with tbe_compiler"
  VERBATIM)
add_library(turbo_media_ivr_schema STATIC ${IVR_GEN_SOURCE})
target_include_directories(
  turbo_media_ivr_schema
  PUBLIC ${IVR_GEN_DIR})
target_link_libraries(
  turbo_media_ivr_schema
  PUBLIC TurboParser::DataBind TurboUtils::Core)
set_target_properties(
  turbo_media_ivr_schema
  PROPERTIES FOLDER "turbomedia/components")
target_link_libraries(turbo_media_ivr PUBLIC turbo_media_ivr_schema)
message(STATUS "IVR codegen: ${TBE_COMPILER_EXECUTABLE}")

# ---------------------------------------------------------------------------
# FlowMQ command/event adapters. FLOWMQ_ROOT is supplied by CMakeUserPresets;
# no deprecated package-root variable or implicit environment fallback exists.
# ---------------------------------------------------------------------------
if(NOT TARGET FlowMQ::FlowMQ)
  message(FATAL_ERROR
    "IVR requires independent FlowMQ::FlowMQ transport")
endif()
target_sources(
  turbo_media_ivr
  PRIVATE src/ivr_flowmq_gateway.c
          src/ivr_room_bridge.c)
target_compile_definitions(turbo_media_ivr PUBLIC TURBO_MEDIA_HAS_FLOWMQ)
target_link_libraries(
  turbo_media_ivr
  PUBLIC FlowMQ::FlowMQ turbo_media_ivr_schema)
# Visible to sibling subdirectories (RoomService app) processed after this one.
set(TURBO_MEDIA_HAS_IVR_FMQ ON CACHE INTERNAL
    "IVR FlowMQ gateway/bridge/adapter available")
message(STATUS "IVR FlowMQ gateway: required and enabled")

function(turbo_media_ivr_configure_test test_name test_source)
  cmake_add_test(
    ${test_name}
    SOURCES ${test_source}
    LIBS turbo_media_ivr TurboUtils::TinyTest
         $<$<PLATFORM_ID:Windows>:ws2_32>
    DEFS IVR_TEST_SCHEMA_PATH="${IVR_TEST_SCHEMA_PATH}"
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
             ${CMAKE_CURRENT_SOURCE_DIR}/include
             ${CMAKE_CURRENT_SOURCE_DIR}/../../speech/include
             ${CMAKE_CURRENT_SOURCE_DIR}/../tests
    FOLDER "turbomedia/tests/ivr")
  set_target_properties(
    ${test_name}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  turbo_media_deploy_windows_runtime(
    ${test_name}
    FlowMQ::FlowMQ
    TurboNet::CoroNet
    TurboParser::DataBind
    TurboParser::Parser
    TurboUtils::Core
    TurboMedia::Speech)
endfunction()

turbo_media_ivr_configure_test(test_ivr_frame tests/test_ivr_frame.c)
turbo_media_ivr_configure_test(test_ivr_protocol tests/test_ivr_protocol.c)
turbo_media_ivr_configure_test(test_ivr_schema tests/test_ivr_schema.c)
turbo_media_ivr_configure_test(test_ivr_acl tests/test_ivr_acl.c)
turbo_media_ivr_configure_test(test_ivr_dtmf_rtp tests/test_ivr_dtmf_rtp.c)
turbo_media_ivr_configure_test(test_ivr_worker tests/test_ivr_worker.c)
turbo_media_ivr_configure_test(test_ivr_media_bot tests/test_ivr_media_bot.c)
turbo_media_ivr_configure_test(test_ivr_media_reconnect
                               tests/test_ivr_media_reconnect.c)
turbo_media_ivr_configure_test(test_ivr_media_supervisor
                               tests/test_ivr_media_supervisor.c)
turbo_media_ivr_configure_test(test_ivr_openai_provider
                               tests/test_ivr_openai_provider.c)
target_sources(test_ivr_openai_provider PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../apps/ivr_worker/ivr_speech_session_factory.c)
target_include_directories(test_ivr_openai_provider PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../apps/ivr_worker)

turbo_media_ivr_configure_test(
  test_ivr_schema_typed tests/test_ivr_schema_typed.c)
turbo_media_ivr_configure_test(
  test_ivr_certificate_identity tests/test_ivr_certificate_identity.c)
turbo_media_ivr_configure_test(
  test_ivr_flowmq tests/test_ivr_flowmq.c)
turbo_media_ivr_configure_test(
  test_ivr_room_bridge tests/test_ivr_room_bridge.c)
turbo_media_ivr_configure_test(
  test_ivr_room_bridge_dedup tests/test_ivr_room_bridge_dedup.c)
