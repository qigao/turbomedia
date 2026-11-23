include(TurboMediaCodecLibraries)
turbo_media_find_codec_libraries(AudioCodec)
turbo_media_find_codec_libraries(VideoCodec)

# The shared registry keeps the existing Codec ABI and uses both backends.
add_library(turbo_media_codec SHARED codec_registry.c)
turbo_media_configure_component(turbo_media_codec Codec)
target_link_libraries(turbo_media_codec PRIVATE
  TurboMediaCodecDeps::AudioCodec TurboMediaCodecDeps::VideoCodec)
# x265 may be a static C++ provider behind its C interface.
set_property(TARGET turbo_media_codec PROPERTY LINKER_LANGUAGE CXX)

install(FILES include/turbo_codec.h
              include/turbo_codec_types.h
              include/turbo_export.h
              include/turbo_player.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

function(turbo_media_install_codec target component public_header)
  cmake_config_target(${target}
    ALIAS TurboMedia::${component} EXPORT_NAME ${component}
    FOLDER "turbomedia/components")
  target_include_directories(${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(TARGETS ${target} EXPORT TurboMedia${component}Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component})
  install(EXPORT TurboMedia${component}Targets NAMESPACE TurboMedia::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia COMPONENT ${component})
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/${public_header}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include/turbo_codec_types.h"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include/turbo_export.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT ${component})
endfunction()

add_subdirectory(audio)
add_subdirectory(video)

if(NOT ANDROID)
  cmake_add_test(turbo_media_test_codec_components
    SOURCES tests/codec_package_consumer/codec_smoke.c
    LIBS TurboMedia::AudioCodec TurboMedia::VideoCodec
    DEFS TEST_AudioCodec TEST_VideoCodec
    FOLDER "turbomedia/tests")
endif()

turbo_media_add_module_test(turbo_media_test_codec tests/test_codec.c
  TurboMedia::Codec)
