cmake_minimum_required(VERSION 3.20)

project(
  TurboIvrWorkerApp
  VERSION 1.0.0
  LANGUAGES C)

# Standalone IVR worker executable. The parent IVR module requires the FlowMQ
# gateway/subscriber before this directory is added.
add_executable(ivr_worker
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
    TurboHttp::Iris
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

turbo_media_deploy_windows_runtime(
  ivr_worker TurboHttp::Iris TurboNet::CoroNet RulesForge::rules_forge)

# Dry-run smoke test: validates config and exercises worker/session/content
# creation without touching FlowMQ.
file(
  TO_CMAKE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/content"
  IVR_WORKER_CONTENT_ROOT)
add_test(
  NAME turbo_media_test_ivr_worker_dry_run
  COMMAND ivr_worker --dry-run --content-root "${IVR_WORKER_CONTENT_ROOT}"
          --assign room-42,call-42,conference-greeting)
set_tests_properties(
  turbo_media_test_ivr_worker_dry_run
  PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin TIMEOUT 30)

add_test(
  NAME turbo_media_test_ivr_worker_rejects_active_plaintext
  COMMAND ${CMAKE_COMMAND}
          -DPROGRAM=$<TARGET_FILE:ivr_worker>
          -DCONTENT_ROOT=${IVR_WORKER_CONTENT_ROOT}
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
          $<TARGET_FILE:ivr_worker> --dry-run --content-root
          "${IVR_WORKER_CONTENT_ROOT}")
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
  LIBS TurboUtils::TinyTest TurboUtils::Core
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  FOLDER "turbomedia/tests/ivr")
set_target_properties(test_ivr_worker_health
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  test_ivr_worker_http
  SOURCES test_ivr_worker_http.c
          ivr_worker_health.c
          ivr_worker_metrics.c
          ivr_worker_http.c
          ../../ivr/src/ivr_http_media_client.c
  LIBS TurboUtils::TinyTest TurboUtils::Core TurboHttp::Iris TurboNet::CoroNet
       $<$<PLATFORM_ID:Windows>:ws2_32>
       $<$<PLATFORM_ID:Linux>:pthread>
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/../../ivr/src
           ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  FOLDER "turbomedia/tests/ivr")
set_target_properties(test_ivr_worker_http
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
turbo_media_deploy_windows_runtime(
  test_ivr_worker_http TurboHttp::Iris TurboNet::CoroNet)

install(FILES config/ivr_worker.toml.example
  DESTINATION etc/turbonet COMPONENT IvrWorker)
if(WIN32)
  install(
    FILES $<TARGET_FILE:TurboHttp::Iris>
          $<TARGET_FILE:TurboNet::CoroNet>
          $<$<TARGET_EXISTS:RulesForge::rules_forge>:$<TARGET_FILE:RulesForge::rules_forge>>
    DESTINATION bin
    COMPONENT IvrWorker)
endif()
