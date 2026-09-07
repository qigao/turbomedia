cmake_minimum_required(VERSION 3.20)

project(
  TurboIvrWorkerApp
  VERSION 1.0.0
  LANGUAGES C)

# Standalone IVR worker executable. The parent IVR module requires the CHTTP H1 WebSocket
# typed WebSocket control gateway before this directory is added.
add_executable(ivr_worker
  ivr_worker_control.c
  ivr_worker_health.c
  ivr_worker_metrics.c
  ivr_worker_http.c
  ivr_speech_session_factory.c
  main.c
)

target_include_directories(ivr_worker
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/include)

target_link_libraries(ivr_worker
  PRIVATE
    turbo_media_ivr
    turbo_media_ivr_rtc
    TurboMedia::RTC
    Salts::CHTTP
    Salts::TomlParser
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>)

target_compile_options(ivr_worker
  PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

set_target_properties(ivr_worker
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "ivr_worker"
    FOLDER "Applications")

# Dry-run smoke test: validates config and exercises media-call admission
# without touching CHTTP H1 WebSocket.
add_test(
  NAME turbo_media_test_ivr_worker_dry_run
  COMMAND ivr_worker --dry-run
          --assign session-42,dialog-42,room-42,call-42)
set_tests_properties(
  turbo_media_test_ivr_worker_dry_run
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)

add_test(
  NAME turbo_media_test_ivr_worker_media_input_timeout_max
  COMMAND ${CMAKE_COMMAND} -E env
          "IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MS=3600000"
          $<TARGET_FILE:ivr_worker> --dry-run)
set_tests_properties(
  turbo_media_test_ivr_worker_media_input_timeout_max
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)

add_test(
  NAME turbo_media_test_ivr_worker_rejects_media_input_timeout_over_max
  COMMAND ${CMAKE_COMMAND} -E env
          "IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MS=3600001"
          $<TARGET_FILE:ivr_worker> --dry-run)
set_tests_properties(
  turbo_media_test_ivr_worker_rejects_media_input_timeout_over_max
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30 WILL_FAIL TRUE)

add_test(
  NAME turbo_media_test_ivr_worker_rejects_active_plaintext
  COMMAND ${CMAKE_COMMAND}
          -DPROGRAM=$<TARGET_FILE:ivr_worker>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_active_plaintext_rejected.cmake)
set_tests_properties(
  turbo_media_test_ivr_worker_rejects_active_plaintext
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             TIMEOUT 30)

add_test(
  NAME turbo_media_test_ivr_worker_log_redaction
  COMMAND ${CMAKE_COMMAND} -E env
          "IVR_OPENAI_BASE_URL=https://user:ivr-log-secret@example.invalid/v1?token=ivr-url-secret"
          "OPENAI_API_KEY=ivr-api-key-secret"
          $<TARGET_FILE:ivr_worker> --dry-run)
set_tests_properties(
  turbo_media_test_ivr_worker_log_redaction
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             TIMEOUT 30
             FAIL_REGULAR_EXPRESSION
             "ivr-log-secret;ivr-url-secret;ivr-api-key-secret")

message(STATUS "IVR Worker App: ${CMAKE_CURRENT_SOURCE_DIR}")

cmake_add_test(
  test_ivr_worker_health
  SOURCES test_ivr_worker_health.c ivr_worker_health.c
  LIBS Salts::TinyTest Salts::Core
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  FOLDER "turbomedia/tests/ivr")
set_target_properties(test_ivr_worker_health
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  test_ivr_worker_control
  SOURCES test_ivr_worker_control.c ivr_worker_control.c
  LIBS Salts::TinyTest Salts::Core
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  FOLDER "turbomedia/tests/ivr")
set_target_properties(test_ivr_worker_control
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  test_ivr_worker_http
  SOURCES test_ivr_worker_http.c
          ivr_worker_health.c
          ivr_worker_metrics.c
          ivr_worker_http.c
          ../../ivr/src/ivr_http_media_client.c
  LIBS Salts::TinyTest Salts::Core Salts::CHTTP Salts::CNet
       $<$<PLATFORM_ID:Linux>:pthread>
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
           ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  FOLDER "turbomedia/tests/ivr")
set_target_properties(test_ivr_worker_http
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

install(FILES config/ivr_worker.toml.example
  DESTINATION etc/turbomedia COMPONENT IvrWorker)
if(WIN32)
  install(
    FILES $<TARGET_FILE:Salts::CHTTP>
          $<TARGET_FILE:Salts::CNet>
          $<TARGET_FILE:Salts::DataBind>
          $<$<TARGET_EXISTS:RulesForge::rules_forge>:$<TARGET_FILE:RulesForge::rules_forge>>
    DESTINATION bin
    COMPONENT IvrWorker)
endif()
