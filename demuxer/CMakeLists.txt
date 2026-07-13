# Demuxer Module

target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/demuxer_registry.c)

# FLV Demuxer
if(TURBO_MEDIA_ENABLE_FLV)
  # FLV demuxer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/flv_demuxer.c)
  
  # FLV demuxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_demuxer_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_demuxer_script.c
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_reader.c
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_parser.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/flv)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_FLV)
endif()

# MP4 Demuxer
if(TURBO_MEDIA_ENABLE_MP4)
  # MP4 demuxer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mp4_demuxer.c)
  
  # MP4 demuxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mov/mov-reader.c
    ${CMAKE_CURRENT_LIST_DIR}/mov/fmp4-reader.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mov)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MP4)
endif()

# MKV Demuxer
if(TURBO_MEDIA_ENABLE_MKV)
  # MKV demuxer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mkv_demuxer.c)
  
  # MKV demuxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mkv/mkv-reader.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mkv)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MKV)
endif()

# MPEG-TS/PS Demuxer
if(TURBO_MEDIA_ENABLE_MPEG)
  # MPEG-TS demuxer
  target_sources(turbo_media PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/mpegts_demuxer.c)
  
  # MPEG-TS demuxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ts-dec.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-pes-dec.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-pes-internal.c)
  
  # MPEG-PS demuxer
  target_sources(turbo_media PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/mpegps_demuxer.c)
  
  # MPEG-PS demuxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ps-dec.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ps-internal.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MPEG)
endif()

# Include directory
target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:include/turbomedia/demuxer>)
