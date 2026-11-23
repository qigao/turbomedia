cmake_add_grammar(
  TURBO_RTSP
  LEXER_RE parser/rtsp_lexer.re
  GRAMMAR_Y parser/rtsp_grammar.y
  LEXER_DEPENDS_ON_GRAMMAR
  FOLDER "turbomedia/streaming/codegen")

add_library(
  turbo_media_rtsp SHARED
  turbo_rtsp.c
  parser/turbo_rtsp_parser.c
  parser/turbo_rtsp_rtp.c
  parser/turbo_rtsp_sdp.c
  ${TURBO_RTSP_LEXER_GEN}
  ${TURBO_RTSP_GRAMMAR_GEN})
turbo_media_configure_component(turbo_media_rtsp RTSP)
target_include_directories(
  turbo_media_rtsp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(
  turbo_media_rtsp
  PUBLIC Salts::CNet CHttp::Client CHttp::Server Salts::Core
  PRIVATE turbo_media_cnet_adapter CHttp::App Salts::Crypto Threads::Threads)
if(WIN32)
  target_link_libraries(turbo_media_rtsp PRIVATE ws2_32)
endif()
target_compile_definitions(turbo_media_rtsp PUBLIC TURBO_MEDIA_HAS_RTSP)

install(FILES include/turbo_rtsp.h
              parser/turbo_rtsp_parser.h
              parser/turbo_rtsp_message.h
              parser/turbo_rtsp_rtp.h
              parser/turbo_rtsp_sdp.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(TURBO_MEDIA_HAS_DESKTOP_SERVICES)
  add_library(turbo_media_server_rtsp SHARED rtsp_server_adapter.c)
  turbo_media_configure_component(turbo_media_server_rtsp ServerRTSP)
  target_link_libraries(turbo_media_server_rtsp PUBLIC TurboMedia::MediaRuntime TurboMedia::RTSP)
endif()

add_subdirectory(tests)
