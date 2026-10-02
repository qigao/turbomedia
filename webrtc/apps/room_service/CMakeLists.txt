cmake_minimum_required(VERSION 3.20)

project(
  TurboRoomService
  VERSION 1.0.0
  LANGUAGES C
  DESCRIPTION "TurboMedia conference room service"
)

set(ROOM_SERVICE_APP_SOURCES
  src/main.c
  src/config.c
  src/http_api.c
  src/server.c
)
if(TURBO_MEDIA_HAS_IVR_CONTROL)
  list(APPEND ROOM_SERVICE_APP_SOURCES
    src/ivr_control_adapter.c)
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
if(TURBO_MEDIA_HAS_IVR_CONTROL)
  list(APPEND ROOM_SERVICE_CORE_SOURCES
    src/ivr_control_adapter.c)
endif()

add_executable(room_service
  ${ROOM_SERVICE_APP_SOURCES}
  ${ROOM_SERVICE_APP_HEADERS}
)

target_include_directories(room_service
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(TURBO_MEDIA_HAS_IVR_CONTROL)
  target_include_directories(room_service
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_compile_definitions(room_service PRIVATE TURBO_MEDIA_HAS_IVR_CONTROL)
endif()

set(ROOM_SERVICE_LIBS
    TurboMedia::RtcApps
    TurboMedia::Transport
    CHttp::Client CHttp::Server
    turbo_media_auth
    turbo_media_revocation_https
    Salts::Crypto
    OpenSSL::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::TomlParser
    Salts::CYaml
    Salts::DateTimeParser)

if(TURBO_MEDIA_HAS_IVR_CONTROL)
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

install(
  TARGETS room_service
  RUNTIME DESTINATION bin
  COMPONENT RoomService
)
if(WIN32)
  install(
    FILES $<TARGET_FILE:CHttp::Client>
          $<TARGET_FILE:CHttp::Server>
          $<TARGET_FILE:Salts::CNet>
          $<$<TARGET_EXISTS:RulesForge::rules_forge>:$<TARGET_FILE:RulesForge::rules_forge>>
          $<TARGET_FILE:Salts::Mustache>
          $<TARGET_FILE:Salts::DataBind>
          $<TARGET_FILE:Salts::Core>
    DESTINATION bin
    COMPONENT RoomService)
endif()
install(
  FILES config/room_service.toml.example
  DESTINATION etc/turbomedia
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
    TurboMedia::Transport
    CHttp::Client CHttp::Server
    turbo_media_auth
    turbo_media_revocation_https
    turbo_media_tenant_quota
    turbo_media_tenant_quota_wire
    Salts::Crypto
    OpenSSL::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::TomlParser
    Salts::CYaml
    Salts::DateTimeParser
    Salts::TinyTest
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>
)
if(TURBO_MEDIA_HAS_IVR_CONTROL)
  target_include_directories(test_room_service_app
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_compile_definitions(test_room_service_app
    PRIVATE TURBO_MEDIA_HAS_IVR_CONTROL)
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
  TIMEOUT 120
  RESOURCE_LOCK http_port_19434)
set_target_properties(test_room_service_app PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  OUTPUT_NAME "test_room_service_app"
  FOLDER "Applications/tests")

if(TURBO_MEDIA_HAS_IVR_CONTROL)
  add_executable(test_ivr_control_adapter
    tests/test_ivr_control_adapter.c
    src/ivr_control_adapter.c
  )
  target_include_directories(test_ivr_control_adapter
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
      ${CMAKE_SOURCE_DIR}/webrtc/tests
  )
  target_link_libraries(test_ivr_control_adapter
    PRIVATE
      TurboMedia::RtcApps
      TurboMedia::RTC
      turbo_media_ivr
      Salts::TinyTest
      $<$<PLATFORM_ID:Linux>:m>
      $<$<PLATFORM_ID:Linux>:pthread>
  )
  target_compile_definitions(test_ivr_control_adapter
    PRIVATE TURBO_MEDIA_HAS_IVR_CONTROL)
  add_test(NAME test_ivr_control_adapter COMMAND test_ivr_control_adapter)
  set_tests_properties(test_ivr_control_adapter PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT 60
    RESOURCE_LOCK ivr_control_ws_port_17715)
  set_target_properties(test_ivr_control_adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "test_ivr_control_adapter"
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
    Salts::TinyTest
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
    Salts::TinyTest
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
set(TEST_IVR_WHIP_TRANSPORT_TIMEOUT_SECONDS 240)
set_tests_properties(test_ivr_whip_transport PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  TIMEOUT ${TEST_IVR_WHIP_TRANSPORT_TIMEOUT_SECONDS})
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
  LIBS turbo_media_auth Salts::TinyTest Salts::Core Salts::TomlParser
  DEFS ROOM_SERVICE_CONFIG_EXAMPLE_PATH="${ROOM_SERVICE_CONFIG_EXAMPLE_PATH}"
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
  FOLDER "Applications/tests")
set_target_properties(
  turbo_media_test_room_service_config
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
