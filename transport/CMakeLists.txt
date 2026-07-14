# Transport Module

# RTP Transport
if(TURBO_MEDIA_ENABLE_RTP)
  # RTP 核心实现
  file(GLOB RTP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rtp/*.c)
  target_sources(turbo_media PRIVATE ${RTP_SOURCES})
  
  # RTP 扩展
  file(GLOB RTP_EXT_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rtp/rtpext/*.c)
  target_sources(turbo_media PRIVATE ${RTP_EXT_SOURCES})
  
  # RTP Payload 处理器
  file(GLOB RTP_PAYLOAD_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rtp/payload/*.c)
  target_sources(turbo_media PRIVATE ${RTP_PAYLOAD_SOURCES})
  
  target_include_directories(turbo_media PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/rtp
    ${CMAKE_CURRENT_LIST_DIR}/rtp/include
    ${CMAKE_CURRENT_LIST_DIR}/rtp/rtpext
    ${CMAKE_CURRENT_LIST_DIR}/rtp/payload)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_RTP)
endif()

# SIP Transport
if(TURBO_MEDIA_ENABLE_SIP)
  # SIP 核心实现（递归包含所有子目录）
  file(GLOB_RECURSE SIP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/sip/*.c)
  target_sources(turbo_media PRIVATE ${SIP_SOURCES})
  
  target_include_directories(turbo_media PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/sip
    ${CMAKE_CURRENT_LIST_DIR}/sip/include)
  
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_SIP)
  
  # 链接 CoroNet
  if(TARGET TurboNet::CoroNet)
    target_link_libraries(turbo_media PRIVATE TurboNet::CoroNet)
  endif()
endif()
