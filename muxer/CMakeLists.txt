# Muxer Module

target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/muxer_registry.c)

# FLV Muxer
if(TURBO_MEDIA_ENABLE_FLV)
  # FLV muxer wrapper
  target_sources(turbo_media PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/flv_muxer.c)
  
  # FLV muxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_muxer_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/flv/flv_writer.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/flv)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_FLV)
endif()

# MP4 Muxer
if(TURBO_MEDIA_ENABLE_MP4)
  # MP4 muxer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mp4_muxer.c)
  
  # MP4 muxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mov/mov-writer.c
    ${CMAKE_CURRENT_LIST_DIR}/mov/fmp4-writer.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mov)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MP4)
endif()

# MKV Muxer
if(TURBO_MEDIA_ENABLE_MKV)
  # MKV muxer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mkv_muxer.c)
  
  # MKV muxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mkv/mkv-writer.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mkv)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MKV)
endif()

# MPEG-TS/PS Muxer
if(TURBO_MEDIA_ENABLE_MPEG)
  # MPEG-TS muxer
  target_sources(turbo_media PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/mpegts_muxer.c)
  
  # MPEG-TS muxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ts.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ts-enc.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-pes-enc.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-element-descriptor.c)
  
  # MPEG-PS muxer
  target_sources(turbo_media PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/mpegps_muxer.c)
  
  # MPEG-PS muxer 内部实现
  target_sources(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ps.c
    ${CMAKE_CURRENT_LIST_DIR}/mpeg/mpeg-ps-enc.c)
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/mpeg)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_MPEG)
endif()

# Include directory
target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:include/turbomedia/muxer>)
