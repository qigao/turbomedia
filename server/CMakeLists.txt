add_library(turbo_media_server SHARED server_runtime.c)
turbo_media_configure_component(turbo_media_server Server)
target_include_directories(
  turbo_media_server
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
  turbo_media_server
  PUBLIC TurboMedia::Core TurboMedia::Transport
  PRIVATE Salts::Core)
