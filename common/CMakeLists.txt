# Common Utilities and Helpers

# FLV 公共工具
target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/flv/turbo_flv_header.c)

target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/flv>
  $<INSTALL_INTERFACE:include/turbomedia/flv>)

# MPEG 公共工具
target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/mpeg/turbo_mpeg_util.c)

target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mpeg>
  $<INSTALL_INTERFACE:include/turbomedia/mpeg>)

# 编解码器辅助工具
file(GLOB CODEC_HELPER_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/codec_helpers/*.c)
target_sources(turbo_media PRIVATE ${CODEC_HELPER_SOURCES})
target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/codec_helpers>
  $<INSTALL_INTERFACE:include/turbomedia/codec_helpers>)

# MOV/MP4 公共组件（box解析器）
if(TURBO_MEDIA_ENABLE_MP4)
  file(GLOB MOV_COMMON_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/mov/*.c)
  target_sources(turbo_media PRIVATE ${MOV_COMMON_SOURCES})
  target_include_directories(turbo_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mov>
    $<INSTALL_INTERFACE:include/turbomedia/mov>)
endif()

# MKV/WebM 公共组件（EBML解析器）
if(TURBO_MEDIA_ENABLE_MKV)
  file(GLOB MKV_COMMON_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/mkv/*.c)
  target_sources(turbo_media PRIVATE ${MKV_COMMON_SOURCES})
  target_include_directories(turbo_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mkv>
    $<INSTALL_INTERFACE:include/turbomedia/mkv>)
endif()
