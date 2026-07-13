function(turbo_media_add_webrtc_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(
    ${name}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${Stb_INCLUDE_DIR})
  target_link_libraries(
    ${name}
    PRIVATE TurboMedia::DataChannel
            TurboMedia::SDP
            TurboUtils::TinyTest
            TurboUtils::Core
            TurboUtils::Parser)
  add_test(NAME ${name} COMMAND ${name})
  set_target_properties(
    ${name}
    PROPERTIES FOLDER "turbomedia/tests/webrtc"
               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endfunction()

turbo_media_add_webrtc_test(turbo_media_test_sdp_parser test_sdp_parser.c)
