cmake_minimum_required(VERSION 3.20)

project(
  TurboRoomService
  VERSION 1.0.0
  LANGUAGES C
  DESCRIPTION "TurboNet conference room service"
)

set(ROOM_SERVICE_APP_SOURCES
  src/main.c
  src/config.c
  src/http_api.c
  src/server.c
)
if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND ROOM_SERVICE_APP_SOURCES src/room_workflow_adapter.c)
endif()
if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_APP_SOURCES src/ivr_fmq_adapter.c)
endif()

set(ROOM_SERVICE_APP_HEADERS
  include/room_service/config.h
  include/room_service/http_api.h
  include/room_service/server.h
)

set(ROOM_SERVICE_CORE_SOURCES
  src/config.c
  src/http_api.c
  src/server.c
)
if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND ROOM_SERVICE_CORE_SOURCES src/room_workflow_adapter.c)
endif()
if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_CORE_SOURCES src/ivr_fmq_adapter.c)
endif()

add_executable(room_service
  ${ROOM_SERVICE_APP_SOURCES}
  ${ROOM_SERVICE_APP_HEADERS}
)

target_include_directories(room_service
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(TURBO_MEDIA_HAS_IVR_FMQ)
  target_include_directories(room_service
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_compile_definitions(room_service PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
endif()

set(ROOM_SERVICE_LIBS
    TurboMedia::RtcApps
    TurboHttp::HttpClient
    TurboHttp::Iris
    turbo_media_auth
    TurboNet::Crypto
    TurboUtils::Core
    TurboUtils::Parser)

if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND ROOM_SERVICE_LIBS TurboRTC::RtcSessionWorkflow)
endif()
if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_LIBS turbo_media_ivr)
endif()

target_link_libraries(room_service
  PRIVATE
    ${ROOM_SERVICE_LIBS}
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>
)

target_compile_options(room_service
  PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

set_target_properties(room_service
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "room_service"
    FOLDER "Applications"
)

turbo_media_deploy_windows_runtime(
  room_service TurboHttp::Iris TurboNet::CoroNet RulesForge::rules_forge)

install(
  TARGETS room_service
  RUNTIME DESTINATION bin
  COMPONENT RoomService
)
if(WIN32)
  install(
    FILES $<TARGET_FILE:TurboHttp::Iris>
          $<TARGET_FILE:TurboNet::CoroNet>
          $<$<TARGET_EXISTS:RulesForge::rules_forge>:$<TARGET_FILE:RulesForge::rules_forge>>
    DESTINATION bin
    COMPONENT RoomService)
endif()
install(
  FILES config/room_service.toml.example
  DESTINATION etc/turbonet
  COMPONENT RoomService
)

add_test(
  NAME turbo_media_test_room_service_dry_run
  COMMAND room_service --config
          ${CMAKE_CURRENT_SOURCE_DIR}/config/room_service.toml.example --dry-run)
set_tests_properties(
  turbo_media_test_room_service_dry_run
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 10)

add_test(
  NAME turbo_media_test_room_service_log_redaction
  COMMAND $<TARGET_FILE:room_service> --config
          ${CMAKE_CURRENT_SOURCE_DIR}/config/room_service.toml.example --dry-run
          --sfu-control-url https://user:room-log-secret@example.invalid/control?token=room-url-secret
          --sfu-nodes node-a=https://user:room-node-secret@example.invalid
          --auth-key-id active --auth-secret
          room-auth-secret-0123456789abcdef0123456789abcdef)
set_tests_properties(
  turbo_media_test_room_service_log_redaction
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             TIMEOUT 10
             FAIL_REGULAR_EXPRESSION
             "room-log-secret;room-url-secret;room-node-secret;room-auth-secret")

message(STATUS "Room Service: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Room Service Output: ${CMAKE_BINARY_DIR}/bin/room_service")

add_executable(test_room_service_app
  tests/test_room_service_app.c
  ${ROOM_SERVICE_CORE_SOURCES}
  ../sfu_node/src/config.c
  ../sfu_node/src/http_api.c
  ../sfu_node/src/server.c
)

target_include_directories(test_room_service_app
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sfu_node/include
    ${CMAKE_SOURCE_DIR}/webrtc/tests
)

target_link_libraries(test_room_service_app
  PRIVATE
    TurboMedia::RtcApps
    TurboMedia::RTC
    TurboHttp::HttpClient
    TurboHttp::Iris
    turbo_media_auth
    TurboNet::Crypto
    TurboUtils::Core
    TurboUtils::Parser
    TurboUtils::TinyTest
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>
)
if(ENABLE_RTC_SCXML_WORKFLOW)
  target_link_libraries(test_room_service_app PRIVATE TurboRTC::RtcSessionWorkflow)
endif()
if(TURBO_MEDIA_HAS_IVR_FMQ)
  target_include_directories(test_room_service_app
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_compile_definitions(test_room_service_app
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  target_link_libraries(test_room_service_app PRIVATE turbo_media_ivr)
endif()

target_compile_options(test_room_service_app
  PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

target_compile_definitions(test_room_service_app
  PRIVATE
    ROOM_SERVICE_ENABLE_TEST_HOOKS=1
    ROOM_SERVICE_TEST_TLS_CERT_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-cert.pem"
    ROOM_SERVICE_TEST_TLS_KEY_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-key.pem"
)


add_test(NAME test_room_service_app COMMAND test_room_service_app)
set_tests_properties(test_room_service_app PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  TIMEOUT 120)
set_target_properties(test_room_service_app PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  OUTPUT_NAME "test_room_service_app"
  FOLDER "Applications/tests")

if(TURBO_MEDIA_HAS_IVR_FMQ)
  add_executable(test_ivr_fmq_adapter
    tests/test_ivr_fmq_adapter.c
    src/ivr_fmq_adapter.c
  )
  target_include_directories(test_ivr_fmq_adapter
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
      ${CMAKE_SOURCE_DIR}/webrtc/tests
  )
  target_link_libraries(test_ivr_fmq_adapter
    PRIVATE
      TurboMedia::RtcApps
      TurboMedia::RTC
      turbo_media_ivr
      TurboUtils::TinyTest
      $<$<PLATFORM_ID:Linux>:m>
      $<$<PLATFORM_ID:Linux>:pthread>
  )
  target_compile_definitions(test_ivr_fmq_adapter
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_ivr_fmq_adapter COMMAND test_ivr_fmq_adapter)
  set_tests_properties(test_ivr_fmq_adapter PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT 60)
  set_target_properties(test_ivr_fmq_adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "test_ivr_fmq_adapter"
    FOLDER "Applications/tests")
endif()

if(TURBO_MEDIA_HAS_IVR_FMQ)
  add_executable(test_ivr_worker_e2e
    tests/test_ivr_worker_e2e.c
    src/ivr_fmq_adapter.c
  )
  target_include_directories(test_ivr_worker_e2e
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
      ${CMAKE_SOURCE_DIR}/webrtc/tests
  )
  target_link_libraries(test_ivr_worker_e2e
    PRIVATE
      TurboMedia::RtcApps
      TurboMedia::RTC
      turbo_media_ivr
      TurboUtils::TinyTest
      $<$<PLATFORM_ID:Linux>:m>
      $<$<PLATFORM_ID:Linux>:pthread>
  )
  target_compile_definitions(test_ivr_worker_e2e
    PRIVATE
      TURBO_MEDIA_HAS_IVR_FMQ
      IVR_TEST_CONTENT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/content")
  add_test(NAME test_ivr_worker_e2e COMMAND test_ivr_worker_e2e)
  set_tests_properties(test_ivr_worker_e2e PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT 60)
  set_target_properties(test_ivr_worker_e2e PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "test_ivr_worker_e2e"
    FOLDER "Applications/tests")
  turbo_media_deploy_windows_runtime(
    test_ivr_worker_e2e TurboNet::CoroNet RulesForge::rules_forge)
endif()

if(TURBO_MEDIA_HAS_IVR_FMQ)
  add_executable(ivr_dispatch_worker_probe
    tests/ivr_dispatch_worker_probe.c)
  target_include_directories(ivr_dispatch_worker_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(ivr_dispatch_worker_probe PRIVATE turbo_media_ivr)
  set_target_properties(ivr_dispatch_worker_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")

  add_executable(test_ivr_dispatch_processes
    tests/test_ivr_dispatch_processes.c
  )
  target_include_directories(test_ivr_dispatch_processes
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
      ${CMAKE_SOURCE_DIR}/webrtc/tests
  )
  target_link_libraries(test_ivr_dispatch_processes
    PRIVATE
      turbo_media_ivr
      TurboUtils::TinyTest
      $<$<PLATFORM_ID:Linux>:m>
      $<$<PLATFORM_ID:Linux>:pthread>
  )
  target_compile_definitions(test_ivr_dispatch_processes
    PRIVATE
      TURBO_MEDIA_HAS_IVR_FMQ
      TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
      IVR_TEST_CONTENT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/content"
      ROOM_SERVICE_BIN="$<TARGET_FILE_NAME:room_service>"
      IVR_WORKER_BIN="$<TARGET_FILE_NAME:ivr_worker>"
      IVR_DISPATCH_PROBE_BIN="$<TARGET_FILE_NAME:ivr_dispatch_worker_probe>")
  if(WIN32)
    target_link_libraries(test_ivr_dispatch_processes PRIVATE ws2_32)
  endif()
  add_test(NAME test_ivr_dispatch_processes COMMAND test_ivr_dispatch_processes)
  add_dependencies(test_ivr_dispatch_processes
    room_service ivr_worker ivr_dispatch_worker_probe)
  set_tests_properties(test_ivr_dispatch_processes PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT 120)
  set_target_properties(test_ivr_dispatch_processes PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "test_ivr_dispatch_processes"
    FOLDER "Applications/tests")
endif()

# SFU minimal environment smoke (spawns the real sfu_node executable).
add_executable(test_sfu_minimal_environment
  tests/test_sfu_minimal_environment.c
)
target_include_directories(test_sfu_minimal_environment
  PRIVATE
    ${CMAKE_SOURCE_DIR}/webrtc/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src)
target_link_libraries(test_sfu_minimal_environment
  PRIVATE
    turbo_media_ivr
    TurboUtils::TinyTest
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>)
target_compile_definitions(test_sfu_minimal_environment
  PRIVATE
    TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
    SFU_NODE_BIN="sfu_node")
if(WIN32)
  target_link_libraries(test_sfu_minimal_environment PRIVATE ws2_32)
endif()
add_test(NAME test_sfu_minimal_environment COMMAND test_sfu_minimal_environment)
set_tests_properties(test_sfu_minimal_environment PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  TIMEOUT 60)
set_target_properties(test_sfu_minimal_environment PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  OUTPUT_NAME "test_sfu_minimal_environment"
  FOLDER "Applications/tests")

# Real WHIP audio transport (ivr_whip_transport) against a live SFU node.
add_executable(test_ivr_whip_transport
  tests/test_ivr_whip_transport.c
)
target_include_directories(test_ivr_whip_transport
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
    ${CMAKE_SOURCE_DIR}/webrtc/tests)
target_link_libraries(test_ivr_whip_transport
  PRIVATE
    turbo_media_ivr_rtc
    TurboUtils::TinyTest
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>)
add_dependencies(test_ivr_whip_transport sfu_node)
target_compile_definitions(test_ivr_whip_transport
  PRIVATE
    TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
    SFU_NODE_BIN="sfu_node")
if(WIN32)
  target_link_libraries(test_ivr_whip_transport PRIVATE ws2_32)
endif()
add_test(NAME test_ivr_whip_transport COMMAND test_ivr_whip_transport)
set_tests_properties(test_ivr_whip_transport PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  TIMEOUT 120)
set_target_properties(test_ivr_whip_transport PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  OUTPUT_NAME "test_ivr_whip_transport"
  FOLDER "Applications/tests")

file(
  TO_CMAKE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/config/room_service.toml.example"
  ROOM_SERVICE_CONFIG_EXAMPLE_PATH)
cmake_add_test(
  turbo_media_test_room_service_config
  SOURCES tests/test_room_service_config.c src/config.c
  LIBS turbo_media_auth TurboUtils::TinyTest TurboUtils::Core TurboUtils::Parser
  DEFS ROOM_SERVICE_CONFIG_EXAMPLE_PATH="${ROOM_SERVICE_CONFIG_EXAMPLE_PATH}"
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
  FOLDER "Applications/tests")
set_target_properties(
  turbo_media_test_room_service_config
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
