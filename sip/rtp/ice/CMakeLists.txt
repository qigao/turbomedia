add_library(turbo_media_rtp_ice STATIC turbo_rtp_ice.c)
turbo_media_configure_component(turbo_media_rtp_ice RTPICE STANDALONE)
set_target_properties(turbo_media_rtp_ice PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(turbo_media_rtp_ice
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_rtp_ice
  PUBLIC TurboMedia::SDP SaltsNet::ICE
  PRIVATE TurboMedia::RTP Salts::Core)

install(FILES include/turbo_rtp_ice.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT RTPICE)
