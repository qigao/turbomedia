add_library(turbo_media_runtime SHARED media_source.c)
turbo_media_configure_component(turbo_media_runtime MediaRuntime)
target_include_directories(
  turbo_media_runtime
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_runtime PRIVATE Salts::Core CMeta::CSTL)

install(FILES include/turbo_media_source.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

turbo_media_add_module_test(turbo_media_test_media_source tests/test_media_source.c
  TurboMedia::MediaRuntime)

if(TURBO_MEDIA_HAS_DESKTOP_SERVICES)
  target_sources(turbo_media_runtime PRIVATE server_runtime.c)
  install(FILES include/turbo_media_server.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  turbo_media_add_module_test(turbo_media_test_server_runtime tests/test_server_runtime.c
    TurboMedia::MediaRuntime)
endif()
