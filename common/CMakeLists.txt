file(GLOB TURBO_MEDIA_CODEC_HELPER_SOURCES CONFIGURE_DEPENDS codec_helpers/*.c)
file(GLOB TURBO_MEDIA_MOV_COMMON_SOURCES CONFIGURE_DEPENDS mov/*.c)
list(FILTER TURBO_MEDIA_MOV_COMMON_SOURCES EXCLUDE REGEX "[/\\\\]mov-(stsd|tx3g)\\.c$")
file(GLOB TURBO_MEDIA_MKV_COMMON_SOURCES CONFIGURE_DEPENDS mkv/*.c)

add_library(
  turbo_media_common_objects OBJECT
  container_io.c
  flv/turbo_flv_header.c
  mpeg/turbo_mpeg_util.c
  ${TURBO_MEDIA_CODEC_HELPER_SOURCES}
  ${TURBO_MEDIA_MOV_COMMON_SOURCES}
  ${TURBO_MEDIA_MKV_COMMON_SOURCES})
target_include_directories(
  turbo_media_common_objects
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/flv
         ${CMAKE_CURRENT_SOURCE_DIR}/mpeg
         ${CMAKE_CURRENT_SOURCE_DIR}/codec_helpers
         ${CMAKE_CURRENT_SOURCE_DIR}/mov
         ${CMAKE_CURRENT_SOURCE_DIR}/mkv
         ${PROJECT_SOURCE_DIR}/media/include)
target_link_libraries(turbo_media_common_objects PUBLIC Salts::Core Salts::CSTL)
set_target_properties(turbo_media_common_objects PROPERTIES FOLDER "turbomedia/components")
