add_library(turbo_media_test_helpers STATIC helpers.c)
target_include_directories(turbo_media_test_helpers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(turbo_media_test_helpers PROPERTIES FOLDER "turbomedia/tests")

turbo_media_add_module_test(turbo_media_test_capture test_capture.c Salts::Media)
turbo_media_add_module_test(turbo_media_test_integration test_integration.c
  TurboMedia::Codec Salts::Media)
turbo_media_add_module_test(turbo_media_test_benchmark test_benchmark.c
  turbo_media_test_helpers TurboMedia::Codec Salts::Media)
if(NOT ANDROID)
  set_tests_properties(turbo_media_test_benchmark PROPERTIES LABELS performance)
endif()
