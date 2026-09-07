add_library(turbo_media_core SHARED media_source.c)
turbo_media_configure_component(turbo_media_core Core)
target_include_directories(
  turbo_media_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_core PRIVATE Salts::Core Salts::CSTL)
