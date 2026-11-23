cmake_add_grammar(
  TURBO_MEDIA_SDP
  LEXER_RE turbo_sdp_parser.re
  LEXER_OUTPUT turbo_sdp_parser_gen.c
  FOLDER "turbomedia/transport/codegen")

add_library(turbo_media_sdp STATIC
  turbo_sdp.c ${TURBO_MEDIA_SDP_LEXER_GEN})
turbo_media_configure_component(turbo_media_sdp SDP STANDALONE)
target_include_directories(turbo_media_sdp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(turbo_media_sdp PRIVATE Salts::Core)

install(FILES include/turbo_sdp.h ${PROJECT_SOURCE_DIR}/media/codec/include/turbo_export.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT SDP)

add_executable(turbo_media_test_sdp_parser tests/test_sdp_parser.c)
target_link_libraries(turbo_media_test_sdp_parser
  PRIVATE Salts::TinyTest Salts::Core TurboMedia::SDP)
set_target_properties(turbo_media_test_sdp_parser
  PROPERTIES FOLDER "turbomedia/tests/transport")
if(NOT ANDROID)
  add_test(NAME turbo_media_test_sdp_parser COMMAND turbo_media_test_sdp_parser)
endif()
