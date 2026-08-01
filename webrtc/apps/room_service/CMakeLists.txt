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

add_executable(room_service
  ${ROOM_SERVICE_APP_SOURCES}
  ${ROOM_SERVICE_APP_HEADERS}
)

target_include_directories(room_service
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(ROOM_SERVICE_LIBS
    TurboRTCApps::Conference
    TurboHttp::HttpClient
    TurboHttp::Iris
    turbo_media_auth
    TurboNet::Crypto
    TurboUtils::Core
    TurboUtils::Parser)

if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND ROOM_SERVICE_LIBS TurboRTC::RtcSessionWorkflow)
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
    TurboRTCApps::Conference
    TurboMedia::RTC
    TurboRTCApps::Recorder
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
