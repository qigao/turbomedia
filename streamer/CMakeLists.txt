# Streamer Module

target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/streamer_registry.c)

# HLS Streamer
if(TURBO_MEDIA_ENABLE_HLS)
  # HLS streamer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/hls_streamer.c)
  
  # HLS streamer 内部实现
  file(GLOB HLS_SOURCES ${CMAKE_CURRENT_LIST_DIR}/hls/*.c)
  target_sources(turbo_media PRIVATE ${HLS_SOURCES})
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/hls)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_HLS)
  
  # 链接 TurboHTTP
  if(TARGET TurboHTTP::http_client)
    target_link_libraries(turbo_media PRIVATE TurboHTTP::http_client)
  endif()
endif()

# DASH Streamer
if(TURBO_MEDIA_ENABLE_DASH)
  # DASH streamer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/dash_streamer.c)
  
  # DASH streamer 内部实现
  file(GLOB DASH_SOURCES ${CMAKE_CURRENT_LIST_DIR}/dash/*.c)
  target_sources(turbo_media PRIVATE ${DASH_SOURCES})
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/dash)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_DASH)
  
  # 链接 TurboHTTP
  if(TARGET TurboHTTP::http_client)
    target_link_libraries(turbo_media PRIVATE TurboHTTP::http_client)
  endif()
endif()

# RTMP Streamer
if(TURBO_MEDIA_ENABLE_RTMP)
  # RTMP streamer wrapper
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/rtmp_streamer.c)
  
  # RTMP 核心实现
  file(GLOB RTMP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rtmp/*.c)
  target_sources(turbo_media PRIVATE ${RTMP_SOURCES})
  
  # RTMP AIO 层
  file(GLOB RTMP_AIO_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rtmp/aio/*.c)
  target_sources(turbo_media PRIVATE ${RTMP_AIO_SOURCES})
  
  target_include_directories(turbo_media PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/rtmp
    ${CMAKE_CURRENT_LIST_DIR}/rtmp/include
    ${CMAKE_CURRENT_LIST_DIR}/rtmp/aio)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_RTMP)
  
  # 链接 CoroNet
  if(TARGET TurboNet::CoroNet)
    target_link_libraries(turbo_media PRIVATE TurboNet::CoroNet)
  endif()
endif()

# HTTP-FLV Streamer
if(TURBO_MEDIA_ENABLE_HTTP_FLV)
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/http_flv_streamer.c)
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_HTTP_FLV)
endif()

# Include directory
target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:include/turbomedia/streamer>)
