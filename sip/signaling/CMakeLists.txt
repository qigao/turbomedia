file(GLOB_RECURSE sip_sources CONFIGURE_DEPENDS *.c)

add_library(turbo_media_sip STATIC ${sip_sources})
turbo_media_configure_component(turbo_media_sip SIP STANDALONE NO_MEDIA_HEADERS)
target_include_directories(turbo_media_sip
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/turbo_media/sip>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(turbo_media_sip
  PUBLIC Salts::Core CMeta::CSTL
  PRIVATE Salts::Crypto Threads::Threads)
target_compile_definitions(turbo_media_sip PUBLIC TURBO_MEDIA_HAS_SIP)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbo_media/sip
  COMPONENT SIP FILES_MATCHING PATTERN "*.h")
