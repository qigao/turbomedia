cmake_minimum_required(VERSION 3.20)

project(
  TurboSfuNode
  VERSION 1.0.0
  LANGUAGES C CXX
  DESCRIPTION "TurboNet conference SFU node")

set(SFU_NODE_CORE_SOURCES src/config.c src/http_api.c src/server.c)

set(SFU_NODE_APP_SOURCES src/main.c ${SFU_NODE_CORE_SOURCES})

set(SFU_NODE_APP_HEADERS include/sfu_node/config.h include/sfu_node/http_api.h
                         include/sfu_node/server.h)

add_executable(sfu_node ${SFU_NODE_APP_SOURCES} ${SFU_NODE_APP_HEADERS})

target_include_directories(sfu_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(SFU_NODE_LIBS
  TurboMedia::RtcApps
  TurboMedia::RTC
  Salts::CHTTP
  turbo_media_auth
  SaltsNet::Crypto
  Salts::Core
  Salts::JsonParser
  Salts::TomlParser)

target_link_libraries(
  sfu_node
  PRIVATE ${SFU_NODE_LIBS}
          $<$<PLATFORM_ID:Linux>:m>
          $<$<PLATFORM_ID:Linux>:pthread>)

target_compile_options(
  sfu_node PRIVATE $<$<C_COMPILER_ID:MSVC>:/W4>
                   $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

set_target_properties(
  sfu_node
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
             OUTPUT_NAME "sfu_node"
             FOLDER "Applications")

turbo_media_deploy_windows_runtime(
  sfu_node Salts::CHTTP Salts::CNet RulesForge::rules_forge)

install(TARGETS sfu_node RUNTIME DESTINATION bin COMPONENT SfuNode)
if(WIN32)
  install(
    FILES $<TARGET_FILE:Salts::CHTTP> $<TARGET_FILE:Salts::CNet>
    DESTINATION bin
    COMPONENT SfuNode)
endif()
install(
  FILES config/sfu_node.toml.example
  DESTINATION etc/turbomedia
  COMPONENT SfuNode)

add_test(
  NAME turbo_media_test_sfu_node_dry_run
  COMMAND sfu_node --config
          ${CMAKE_CURRENT_SOURCE_DIR}/config/sfu_node.toml.example --dry-run)
set_tests_properties(
  turbo_media_test_sfu_node_dry_run
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 10)

message(STATUS "SFU Node: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "SFU Node Output: ${CMAKE_BINARY_DIR}/bin/sfu_node")

add_executable(test_sfu_node_app tests/test_sfu_node_app.c
                                 ../../ivr/src/ivr_http_media_client.c
                                 ${SFU_NODE_CORE_SOURCES})

target_include_directories(
  test_sfu_node_app
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/webrtc/src/recording
          ${CMAKE_SOURCE_DIR}/webrtc/tests
          ${CMAKE_SOURCE_DIR}/webrtc/ivr/src)

target_link_libraries(
  test_sfu_node_app
  PRIVATE ${SFU_NODE_LIBS}
          TurboMedia::Demuxer
          Salts::CHTTP
          Salts::TinyTest
          $<$<PLATFORM_ID:Linux>:m>
          $<$<PLATFORM_ID:Linux>:pthread>)

file(TO_CMAKE_PATH
     "${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-cert.pem"
     SFU_NODE_TEST_TLS_CERT_PATH)
file(TO_CMAKE_PATH
     "${CMAKE_SOURCE_DIR}/webrtc/tests/fixtures/tls/localhost-key.pem"
     SFU_NODE_TEST_TLS_KEY_PATH)
target_compile_definitions(
  test_sfu_node_app
  PRIVATE SFU_NODE_TEST_TLS_CERT_PATH="${SFU_NODE_TEST_TLS_CERT_PATH}"
          SFU_NODE_TEST_TLS_KEY_PATH="${SFU_NODE_TEST_TLS_KEY_PATH}")

target_compile_options(
  test_sfu_node_app
  PRIVATE $<$<C_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall
          -Wextra -Wpedantic>)

add_test(NAME test_sfu_node_app COMMAND test_sfu_node_app)
set_tests_properties(test_sfu_node_app PROPERTIES WORKING_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin
                                                  TIMEOUT 120
                                                  RESOURCE_LOCK http_port_19434)
set_target_properties(
  test_sfu_node_app
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
             OUTPUT_NAME "test_sfu_node_app"
             FOLDER "Applications/tests")

file(
  TO_CMAKE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/config/sfu_node.toml.example"
  SFU_NODE_CONFIG_EXAMPLE_PATH)
cmake_add_test(
  turbo_media_test_sfu_node_config
  SOURCES tests/test_sfu_node_config.c src/config.c
  LIBS turbo_media_auth Salts::TinyTest Salts::Core
       Salts::TomlParser
  DEFS SFU_NODE_CONFIG_EXAMPLE_PATH="${SFU_NODE_CONFIG_EXAMPLE_PATH}"
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
  FOLDER "Applications/tests")
set_target_properties(
  turbo_media_test_sfu_node_config
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
