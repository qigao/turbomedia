add_library(
  turbo_media_transport SHARED
  transport_coronet.c
  transport_http.c)
turbo_media_configure_component(turbo_media_transport Transport)
target_include_directories(
  turbo_media_transport
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(
  turbo_media_transport
  PUBLIC TurboNet::CoroNet TurboParser::Parser TurboHttp::HttpClient
  PRIVATE TurboUtils::Core TurboUtils::STL Threads::Threads)
