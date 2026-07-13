find_package(Threads REQUIRED)

set(TURBO_MEDIA_TURBONET_HINTS)
if(TURBONET_ROOT)
  list(APPEND TURBO_MEDIA_TURBONET_HINTS "${TURBONET_ROOT}")
endif()
list(APPEND TURBO_MEDIA_TURBONET_HINTS
     "${PROJECT_SOURCE_DIR}/../turbonet/build"
     "${PROJECT_SOURCE_DIR}/../turbonet")

find_package(TurboNet CONFIG REQUIRED PATHS ${TURBO_MEDIA_TURBONET_HINTS})

find_program(RE2C_EXECUTABLE re2c REQUIRED)

find_program(TURBO_MEDIA_SYSTEM_LEMON lemon)
if(TURBO_MEDIA_SYSTEM_LEMON)
  set(LEMON_EXECUTABLE "${TURBO_MEDIA_SYSTEM_LEMON}")
else()
  add_executable(turbo_media_lemon ${PROJECT_SOURCE_DIR}/tools/lemon/lemon.c)
  set_target_properties(turbo_media_lemon PROPERTIES FOLDER "turbomedia/tools")
  set(LEMON_EXECUTABLE $<TARGET_FILE:turbo_media_lemon>)
  set(LEMON_DEPENDS turbo_media_lemon)
endif()
set(LEMPAR "${PROJECT_SOURCE_DIR}/tools/lemon/lempar.c")

set(TURBO_RTSP_LEXER_RE ${CMAKE_CURRENT_LIST_DIR}/parser/rtsp_lexer.re)
set(TURBO_RTSP_GRAMMAR_Y ${CMAKE_CURRENT_LIST_DIR}/parser/rtsp_grammar.y)
set(TURBO_RTSP_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/rtsp_lexer_gen.c)
set(TURBO_RTSP_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/rtsp_grammar_gen.c)
set(TURBO_RTSP_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/rtsp_grammar_gen.h)
set(TURBO_RTSP_GRAMMAR_Y_GEN ${CMAKE_CURRENT_BINARY_DIR}/rtsp_grammar_gen.y)

add_custom_command(
  OUTPUT ${TURBO_RTSP_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${TURBO_RTSP_LEXER_GEN} ${TURBO_RTSP_LEXER_RE}
  DEPENDS ${TURBO_RTSP_LEXER_RE} ${TURBO_RTSP_GRAMMAR_H}
  COMMENT "Generating RTSP lexer with re2c"
  VERBATIM)

add_custom_command(
  OUTPUT ${TURBO_RTSP_GRAMMAR_GEN} ${TURBO_RTSP_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${TURBO_RTSP_GRAMMAR_Y} ${TURBO_RTSP_GRAMMAR_Y_GEN}
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR} ${TURBO_RTSP_GRAMMAR_Y_GEN}
  DEPENDS ${TURBO_RTSP_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating RTSP parser with lemon"
  VERBATIM)

add_library(
  turbo_media_rtsp SHARED
  ${CMAKE_CURRENT_LIST_DIR}/turbo_rtsp.c
  ${CMAKE_CURRENT_LIST_DIR}/parser/turbo_rtsp_parser.c
  ${CMAKE_CURRENT_LIST_DIR}/parser/turbo_rtsp_rtp.c
  ${CMAKE_CURRENT_LIST_DIR}/parser/turbo_rtsp_sdp.c
  ${TURBO_RTSP_LEXER_GEN}
  ${TURBO_RTSP_GRAMMAR_GEN})

target_include_directories(
  turbo_media_rtsp
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/parser>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/media/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(
  turbo_media_rtsp
  PUBLIC TurboNet::CoroNet TurboNet::Utils)
if(WIN32)
  target_link_libraries(turbo_media_rtsp PRIVATE ws2_32)
endif()
target_compile_features(turbo_media_rtsp PUBLIC c_std_11)
if(MSVC)
  target_compile_options(turbo_media_rtsp PUBLIC $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
target_compile_definitions(turbo_media_rtsp PUBLIC TURBO_MEDIA_HAS_RTSP)
target_compile_definitions(turbo_media_rtsp PRIVATE SHARED_CXX)

set_target_properties(
  turbo_media_rtsp
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             POSITION_INDEPENDENT_CODE ON
             FOLDER "turbomedia/rtsp"
             EXPORT_NAME RTSP)

add_library(TurboMedia::RTSP ALIAS turbo_media_rtsp)
