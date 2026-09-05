cmake_add_grammar(
  TURBO_RTSP
  LEXER_RE parser/rtsp_lexer.re
  GRAMMAR_Y parser/rtsp_grammar.y
  LEXER_DEPENDS_ON_GRAMMAR
  FOLDER "turbomedia/rtsp/codegen")

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
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(
  turbo_media_rtsp
  PUBLIC TurboNet::CoroNet Salts::Core
  PRIVATE OpenSSL::Crypto Threads::Threads)
if(WIN32)
  target_link_libraries(turbo_media_rtsp PRIVATE ws2_32)
endif()
target_compile_definitions(turbo_media_rtsp PUBLIC TURBO_MEDIA_HAS_RTSP)
