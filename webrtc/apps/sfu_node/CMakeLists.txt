cmake_minimum_required(VERSION 3.20)

project(
  TurboSfuNode
  VERSION 1.0.0
  LANGUAGES C CXX
  DESCRIPTION "TurboNet conference SFU node")

set(SFU_NODE_CORE_SOURCES src/config.c src/http_api.c src/server.c)
if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND SFU_NODE_CORE_SOURCES src/rtc_workflow_adapter.c)
endif()

set(SFU_NODE_APP_SOURCES src/main.c ${SFU_NODE_CORE_SOURCES})

set(SFU_NODE_APP_HEADERS include/sfu_node/config.h include/sfu_node/http_api.h
                         include/sfu_node/server.h)

add_executable(sfu_node ${SFU_NODE_APP_SOURCES} ${SFU_NODE_APP_HEADERS})

target_include_directories(sfu_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(SFU_NODE_LIBS
  TurboRTC::Conference
  TurboRTC::MediaEngine
  TurboRTC::Recorder
  TurboHttp::Iris
  TurboNet::Utils)

if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND SFU_NODE_LIBS TurboRTC::RtcSessionWorkflow)
endif()

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

install(TARGETS sfu_node RUNTIME DESTINATION bin COMPONENT SfuNode)

message(STATUS "SFU Node: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "SFU Node Output: ${CMAKE_BINARY_DIR}/bin/sfu_node")

add_executable(test_sfu_node_app tests/test_sfu_node_app.c
                                 ${SFU_NODE_CORE_SOURCES})

target_include_directories(
  test_sfu_node_app
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)

target_link_libraries(
  test_sfu_node_app
  PRIVATE ${SFU_NODE_LIBS}
          TurboHttp::HttpClient
          TurboNet::TinyTest
          $<$<PLATFORM_ID:Linux>:m>
          $<$<PLATFORM_ID:Linux>:pthread>)

target_compile_options(
  test_sfu_node_app
  PRIVATE $<$<C_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall
          -Wextra -Wpedantic>)

add_test(NAME test_sfu_node_app COMMAND test_sfu_node_app)
set_tests_properties(test_sfu_node_app PROPERTIES WORKING_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
set_target_properties(
  test_sfu_node_app
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
             OUTPUT_NAME "test_sfu_node_app"
             FOLDER "Applications/tests")
