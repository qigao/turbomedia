file(GLOB rtp_sources CONFIGURE_DEPENDS *.c)
file(GLOB rtp_extension_sources CONFIGURE_DEPENDS rtpext/*.c)
file(GLOB rtp_payload_sources CONFIGURE_DEPENDS payload/*.c)
list(APPEND rtp_sources ${rtp_extension_sources} ${rtp_payload_sources})

add_library(turbo_media_rtp STATIC ${rtp_sources})
turbo_media_configure_component(turbo_media_rtp RTP STANDALONE)
set_target_properties(turbo_media_rtp PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(turbo_media_rtp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} rtpext payload)
target_link_libraries(turbo_media_rtp PRIVATE Salts::Core)
target_compile_definitions(turbo_media_rtp PUBLIC TURBO_MEDIA_HAS_RTP)

install(FILES include/rtcp-header.h include/turbo_rtp_packet.h
              include/rtp-payload.h include/rtp.h include/rtp-demuxer.h
              ${PROJECT_SOURCE_DIR}/media/codec/include/turbo_export.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT RTP)

add_subdirectory(ice)
