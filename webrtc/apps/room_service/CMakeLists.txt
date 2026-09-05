cmake_minimum_required(VERSION 3.20)

project(
  TurboRoomService
  VERSION 1.0.0
  LANGUAGES C
  DESCRIPTION "TurboNet conference room service"
)

# First-party packages are resolved once at the repository root from their
# explicit environment roots, so every WebRTC subdirectory sees the same targets.
if(NOT TARGET TurboDB::ORM)
  message(FATAL_ERROR
    "RoomService Iris outbox requires TurboDB::ORM")
endif()

turbo_media_find_tbe_compiler(ROOM_SERVICE_TBE_COMPILER_EXECUTABLE)
set(ROOM_SERVICE_OUTBOX_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ROOM_SERVICE_OUTBOX_GEN_DIR}")
set(ROOM_SERVICE_OUTBOX_GEN_HEADER
  "${ROOM_SERVICE_OUTBOX_GEN_DIR}/iris_event_outbox_v1.h")
set(ROOM_SERVICE_OUTBOX_GEN_SOURCE
  "${ROOM_SERVICE_OUTBOX_GEN_DIR}/iris_event_outbox_v1.c")
set(ROOM_SERVICE_LEDGER_GEN_HEADER
  "${ROOM_SERVICE_OUTBOX_GEN_DIR}/iris_command_ledger_v2.h")
set(ROOM_SERVICE_LEDGER_GEN_SOURCE
  "${ROOM_SERVICE_OUTBOX_GEN_DIR}/iris_command_ledger_v2.c")
add_custom_command(
  OUTPUT ${ROOM_SERVICE_OUTBOX_GEN_HEADER} ${ROOM_SERVICE_OUTBOX_GEN_SOURCE}
  COMMAND ${ROOM_SERVICE_TBE_COMPILER_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/schema/iris_event_outbox_v1.schema
          --lang c --output ${ROOM_SERVICE_OUTBOX_GEN_HEADER}
          --source-output ${ROOM_SERVICE_OUTBOX_GEN_SOURCE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/schema/iris_event_outbox_v1.schema
  COMMENT "Generating Iris media event outbox schema"
  VERBATIM)
add_custom_command(
  OUTPUT ${ROOM_SERVICE_LEDGER_GEN_HEADER} ${ROOM_SERVICE_LEDGER_GEN_SOURCE}
  COMMAND ${ROOM_SERVICE_TBE_COMPILER_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/schema/iris_command_ledger_v2.schema
          --lang c --output ${ROOM_SERVICE_LEDGER_GEN_HEADER}
          --source-output ${ROOM_SERVICE_LEDGER_GEN_SOURCE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/schema/iris_command_ledger_v2.schema
  COMMENT "Generating Iris provider command ledger schema"
  VERBATIM)
add_library(room_service_iris_outbox_schema STATIC
  ${ROOM_SERVICE_OUTBOX_GEN_SOURCE}
  ${ROOM_SERVICE_LEDGER_GEN_SOURCE})
target_include_directories(room_service_iris_outbox_schema
  PUBLIC ${ROOM_SERVICE_OUTBOX_GEN_DIR})
target_link_libraries(room_service_iris_outbox_schema
  PUBLIC Salts::DataBind Salts::Core)
set_target_properties(room_service_iris_outbox_schema
  PROPERTIES FOLDER "Applications/components")

set(ROOM_SERVICE_APP_SOURCES
  src/main.c
  src/config.c
  src/http_api.c
  src/server.c
)
if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_APP_SOURCES
    src/iris_event_outbox.c
    src/iris_orm_store.c
    src/iris_command_ledger.c
    src/iris_command_fingerprint.c
    src/iris_flowmq_provider_codec.c
    src/iris_flowmq_provider.c
    src/iris_completion_dispatcher.c
    src/iris_media_reconciler.c
    src/iris_media_bridge.c
    src/iris_room_bridge.c
    src/ivr_fmq_adapter.c)
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
if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_CORE_SOURCES
    src/iris_event_outbox.c
    src/iris_orm_store.c
    src/iris_command_ledger.c
    src/iris_command_fingerprint.c
    src/iris_flowmq_provider_codec.c
    src/iris_flowmq_provider.c
    src/iris_completion_dispatcher.c
    src/iris_media_reconciler.c
    src/iris_media_bridge.c
    src/iris_room_bridge.c
    src/ivr_fmq_adapter.c)
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
    TurboHttp::TurboHttp
    TurboHttp::Iris
    turbo_media_auth
    TurboNet::Crypto
    OpenSSL::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::TomlParser
    Salts::CYaml
    Salts::DateTimeParser)

if(TURBO_MEDIA_HAS_IVR_FMQ)
  list(APPEND ROOM_SERVICE_LIBS
    turbo_media_ivr
    room_service_iris_outbox_schema
    TurboDB::ORM)
  list(APPEND ROOM_SERVICE_LIBS FlowMQ::FlowMQ)
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
  room_service TurboHttp::Iris TurboNet::CoroNet RulesForge::rules_forge
  FlowMQ::FlowMQ Salts::Mustache Salts::DataBind Salts::Core Salts::Capture)

install(
  TARGETS room_service
  RUNTIME DESTINATION bin
  COMPONENT RoomService
)
if(WIN32)
  get_filename_component(ROOM_SERVICE_SQLITE_LIBRARY_DIR
                         "${SQLite3_LIBRARY}" DIRECTORY)
  get_filename_component(ROOM_SERVICE_SQLITE_RUNTIME_ROOT
                         "${ROOM_SERVICE_SQLITE_LIBRARY_DIR}" DIRECTORY)
  set(ROOM_SERVICE_SQLITE_RUNTIME
      "${ROOM_SERVICE_SQLITE_RUNTIME_ROOT}/bin/sqlite3.dll")
  if(NOT EXISTS "${ROOM_SERVICE_SQLITE_RUNTIME}")
    message(FATAL_ERROR
      "SQLite ORM runtime is missing: ${ROOM_SERVICE_SQLITE_RUNTIME}")
  endif()
  install(
    FILES $<TARGET_FILE:TurboHttp::Iris>
          $<TARGET_FILE:TurboNet::CoroNet>
          $<$<TARGET_EXISTS:RulesForge::rules_forge>:$<TARGET_FILE:RulesForge::rules_forge>>
          $<TARGET_FILE:FlowMQ::FlowMQ>
          $<TARGET_FILE:Salts::Mustache>
          $<TARGET_FILE:Salts::DataBind>
          $<TARGET_FILE:Salts::Core>
          $<TARGET_FILE:Salts::Capture>
          ${ROOM_SERVICE_SQLITE_RUNTIME}
    DESTINATION bin
    COMPONENT RoomService)
endif()
install(
  FILES config/room_service.toml.example
        config/room_service.orm.sqlite.yaml.example
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
    TurboHttp::TurboHttp
    TurboHttp::Iris
    turbo_media_auth
    TurboNet::Crypto
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
if(TURBO_MEDIA_HAS_IVR_FMQ)
  target_include_directories(test_room_service_app
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_compile_definitions(test_room_service_app
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  target_link_libraries(test_room_service_app PRIVATE turbo_media_ivr)
  target_link_libraries(test_room_service_app PRIVATE
    room_service_iris_outbox_schema
    TurboDB::ORM)
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
turbo_media_deploy_windows_runtime(
  test_room_service_app FlowMQ::FlowMQ TurboNet::CoroNet
  Salts::Mustache Salts::DataBind Salts::Core Salts::Capture)

if(TURBO_MEDIA_HAS_IVR_FMQ)
  add_executable(test_iris_media_bridge
    tests/test_iris_media_bridge.c
    src/iris_media_bridge.c
    src/iris_command_fingerprint.c
  )
  target_include_directories(test_iris_media_bridge
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include
  )
  target_link_libraries(test_iris_media_bridge
    PRIVATE
      turbo_media_ivr
      TurboNet::Crypto
      Salts::Core
      Salts::JsonParser
      Salts::DateTimeParser
      Salts::TinyTest
  )
  target_compile_definitions(test_iris_media_bridge
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_media_bridge COMMAND test_iris_media_bridge)
  set_tests_properties(test_iris_media_bridge PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT 30)
  set_target_properties(test_iris_media_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "test_iris_media_bridge"
    FOLDER "Applications/tests")

  add_executable(test_iris_flowmq_provider_codec
    tests/test_iris_flowmq_provider_codec.c
    src/iris_flowmq_provider_codec.c)
  target_include_directories(test_iris_flowmq_provider_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_flowmq_provider_codec PRIVATE
    FlowMQ::FlowMQ
    TurboNet::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::TinyTest)
  target_compile_definitions(test_iris_flowmq_provider_codec
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_flowmq_provider_codec
    COMMAND test_iris_flowmq_provider_codec)
  set_tests_properties(test_iris_flowmq_provider_codec PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_flowmq_provider_codec PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")

  add_executable(test_iris_flowmq_provider
    tests/test_iris_flowmq_provider.c
    src/iris_flowmq_provider.c
    src/iris_flowmq_provider_codec.c)
  target_include_directories(test_iris_flowmq_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_flowmq_provider PRIVATE
    FlowMQ::FlowMQ
    TurboNet::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::TinyTest
    $<$<PLATFORM_ID:Windows>:ws2_32>)
  target_compile_definitions(test_iris_flowmq_provider
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_flowmq_provider COMMAND test_iris_flowmq_provider)
  set_tests_properties(test_iris_flowmq_provider PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_flowmq_provider PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")

  add_executable(test_iris_room_bridge
    tests/test_iris_room_bridge.c
    src/iris_room_bridge.c
    src/iris_command_fingerprint.c)
  target_include_directories(test_iris_room_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_room_bridge PRIVATE
    TurboNet::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::DateTimeParser
    Salts::TinyTest)
  target_compile_definitions(test_iris_room_bridge
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_room_bridge COMMAND test_iris_room_bridge)
  set_tests_properties(test_iris_room_bridge PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_room_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")

  add_executable(test_iris_completion_dispatcher
    tests/test_iris_completion_dispatcher.c
    src/iris_completion_dispatcher.c
    src/iris_media_bridge.c
    src/iris_command_fingerprint.c)
  target_include_directories(test_iris_completion_dispatcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_completion_dispatcher PRIVATE
    turbo_media_ivr
    TurboHttp::TurboHttp
    TurboHttp::Iris
    TurboNet::Crypto
    Salts::Core
    Salts::JsonParser
    Salts::DateTimeParser
    Salts::TinyTest)
  target_compile_definitions(test_iris_completion_dispatcher
    PRIVATE
      TURBO_MEDIA_HAS_IVR_FMQ
      ROOM_SERVICE_TEST_TLS_CERT_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-cert.pem"
      ROOM_SERVICE_TEST_TLS_KEY_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-key.pem")
  add_test(NAME test_iris_completion_dispatcher
    COMMAND test_iris_completion_dispatcher)
  set_tests_properties(test_iris_completion_dispatcher PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_completion_dispatcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")

  add_executable(test_iris_media_reconciler
    tests/test_iris_media_reconciler.c
    src/iris_media_reconciler.c
    src/iris_flowmq_provider.c
    src/iris_flowmq_provider_codec.c
    src/iris_event_outbox.c
    src/iris_orm_store.c
    src/ivr_fmq_adapter.c)
  target_include_directories(test_iris_media_reconciler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_media_reconciler PRIVATE
    FlowMQ::FlowMQ
    TurboMedia::RtcApps
    turbo_media_ivr
    room_service_iris_outbox_schema
    TurboHttp::TurboHttp
    TurboNet::Crypto
    OpenSSL::Crypto
    TurboDB::ORM
    Salts::Core
    Salts::JsonParser
    Salts::CYaml
    Salts::TinyTest)
  target_compile_definitions(test_iris_media_reconciler
    PRIVATE TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_media_reconciler
    COMMAND test_iris_media_reconciler)
  set_tests_properties(test_iris_media_reconciler PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_media_reconciler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")
  turbo_media_deploy_windows_runtime(
    test_iris_media_reconciler FlowMQ::FlowMQ TurboNet::CoroNet
    Salts::Mustache Salts::Core)

  add_executable(test_iris_event_outbox
    tests/test_iris_event_outbox.c
    src/iris_event_outbox.c
    src/iris_orm_store.c)
  target_include_directories(test_iris_event_outbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_event_outbox PRIVATE
    room_service_iris_outbox_schema FlowMQ::FlowMQ
    TurboDB::ORM
    Salts::Core Salts::DataBind Salts::CYaml Salts::TinyTest)
  target_compile_definitions(test_iris_event_outbox PRIVATE
    TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_event_outbox COMMAND test_iris_event_outbox)
  set_tests_properties(test_iris_event_outbox PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_event_outbox PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")
  turbo_media_deploy_windows_runtime(
    test_iris_event_outbox FlowMQ::FlowMQ TurboNet::CoroNet
    Salts::Mustache Salts::Core)

  add_executable(test_iris_command_ledger
    tests/test_iris_command_ledger.c
    src/iris_command_ledger.c
    src/iris_orm_store.c)
  target_include_directories(test_iris_command_ledger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)
  target_link_libraries(test_iris_command_ledger PRIVATE
    room_service_iris_outbox_schema
    TurboDB::ORM
    Salts::Core Salts::DataBind Salts::CYaml
    Salts::TinyTest)
  target_compile_definitions(test_iris_command_ledger PRIVATE
    TURBO_MEDIA_HAS_IVR_FMQ)
  add_test(NAME test_iris_command_ledger COMMAND test_iris_command_ledger)
  set_tests_properties(test_iris_command_ledger PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_command_ledger PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")
  turbo_media_deploy_windows_runtime(
    test_iris_command_ledger Salts::Mustache Salts::Core)

  add_executable(test_iris_orm_store
    tests/test_iris_orm_store.c
    src/iris_orm_store.c)
  target_include_directories(test_iris_orm_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_iris_orm_store PRIVATE
    TurboDB::ORM Salts::Core Salts::CYaml Salts::TinyTest)
  add_test(NAME test_iris_orm_store COMMAND test_iris_orm_store)
  set_tests_properties(test_iris_orm_store PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)
  set_target_properties(test_iris_orm_store PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Applications/tests")
  turbo_media_deploy_windows_runtime(
    test_iris_orm_store Salts::Mustache Salts::Core)

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
      Salts::TinyTest
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
  turbo_media_deploy_windows_runtime(
    test_ivr_fmq_adapter FlowMQ::FlowMQ TurboNet::CoroNet
    Salts::Core)
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
    tests/iris_flowmq_process_peer.c
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
      turbo_media_ivr_rtc
      TurboHttp::TurboHttp
      TurboHttp::Iris
      FlowMQ::FlowMQ
      Salts::JsonParser
      Salts::DataBind
      TurboNet::CoroNet
      TurboNet::Crypto
      Salts::Core
      Salts::TinyTest
      $<$<PLATFORM_ID:Linux>:m>
      $<$<PLATFORM_ID:Linux>:pthread>
  )
  target_compile_definitions(test_ivr_dispatch_processes
    PRIVATE
      TURBO_MEDIA_HAS_IVR_FMQ
      TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
      ROOM_SERVICE_BIN="$<TARGET_FILE_NAME:room_service>"
      IVR_WORKER_BIN="$<TARGET_FILE_NAME:ivr_worker>"
      IVR_DISPATCH_PROBE_BIN="$<TARGET_FILE_NAME:ivr_dispatch_worker_probe>"
      SFU_NODE_BIN="$<TARGET_FILE_NAME:sfu_node>"
      ROOM_SERVICE_TEST_TLS_CERT_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-cert.pem"
      ROOM_SERVICE_TEST_TLS_KEY_PATH="${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-key.pem")
  if(WIN32)
    target_link_libraries(test_ivr_dispatch_processes PRIVATE ws2_32)
  endif()
  add_test(NAME test_ivr_dispatch_processes COMMAND test_ivr_dispatch_processes)
  add_dependencies(test_ivr_dispatch_processes
    room_service ivr_worker ivr_dispatch_worker_probe sfu_node)
  set(TEST_IVR_DISPATCH_PROCESSES_TIMEOUT_SECONDS 300)
  set_tests_properties(test_ivr_dispatch_processes PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    TIMEOUT ${TEST_IVR_DISPATCH_PROCESSES_TIMEOUT_SECONDS})
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
