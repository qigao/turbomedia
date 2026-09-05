add_library(turbo_media_crypto SHARED monocypher_crypto.c)
turbo_media_configure_component(turbo_media_crypto Crypto)
target_include_directories(
  turbo_media_crypto
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${PROJECT_SOURCE_DIR}/vendor/monocypher)
target_link_libraries(
  turbo_media_crypto
  PUBLIC Salts::Core
  PRIVATE TurboMediaVendor::Monocypher)
