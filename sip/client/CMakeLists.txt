if(WIN32)
  add_library(turbo_media_sip_client SHARED turbo_sip_client.c)
  set_target_properties(turbo_media_sip_client PROPERTIES
    DEFINE_SYMBOL TURBO_SIP_CLIENT_EXPORTS
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  target_compile_definitions(turbo_media_sip_client PUBLIC TURBO_SIP_CLIENT_SHARED)
else()
  add_library(turbo_media_sip_client STATIC turbo_sip_client.c)
endif()
turbo_media_configure_component(turbo_media_sip_client SIPClient STANDALONE EXPLICIT_EXPORTS)
target_include_directories(turbo_media_sip_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../signaling)
target_link_libraries(turbo_media_sip_client
  PUBLIC TurboMedia::SIP TurboMedia::SDP
  PRIVATE TurboMedia::RTP TurboMedia::AudioCodec Salts::Crypto)

install(FILES include/turbo_sip_client.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT SIPClient)
turbo_media_install_windows_runtime(turbo_media_sip_client SIPClient)
