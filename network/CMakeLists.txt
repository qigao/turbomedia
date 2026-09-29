add_library(
  turbo_media_cnet_adapter STATIC
  internal/turbo_cnet_send_internal.c)
target_include_directories(
  turbo_media_cnet_adapter
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/internal)
target_link_libraries(
  turbo_media_cnet_adapter
  PRIVATE Salts::CNet Salts::Core)
set_target_properties(
  turbo_media_cnet_adapter PROPERTIES FOLDER "turbomedia/components")

add_library(
  turbo_media_transport SHARED
  transport_cnet.c
  transport_http.c
  transport_url.c)
turbo_media_configure_component(turbo_media_transport Transport)
target_include_directories(
  turbo_media_transport
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
  turbo_media_transport
  PUBLIC Salts::CNet CHttp::Client
  PRIVATE turbo_media_cnet_adapter
          Salts::UriParser Salts::Core Salts::CSTL Threads::Threads)
