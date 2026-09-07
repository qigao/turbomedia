# 共享测试辅助源文件
set(TEST_HELPER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.h
)

# 使用 glob 查找所有测试文件（test_*.c）
file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

# 为每个测试文件创建可执行目标
foreach(TEST_SOURCE ${TEST_SOURCES})
  # 提取测试名称（去掉路径和扩展名）
  get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

  if(TEST_NAME STREQUAL "test_mobile_optimizer" AND NOT ANDROID)
    continue()
  endif()

  if(TEST_NAME STREQUAL "test_android_capture" AND NOT ANDROID)
    continue()
  endif()
  
  # 根据测试类型确定链接库
  set(TEST_LINK_LIBS Salts::TinyTest)
  if(UNIX)
    list(APPEND TEST_LINK_LIBS m)
  endif()
  
  if(TEST_NAME MATCHES "rtsp")
    # RTSP 相关测试
    list(APPEND TEST_LINK_LIBS TurboMedia::RTSP TurboMedia::Core
                               TurboMedia::ServerRTSP)
    if(TEST_NAME STREQUAL "test_rtsp")
      list(APPEND TEST_LINK_LIBS OpenSSL::Crypto)
    endif()
  elseif(TEST_NAME STREQUAL "test_crypto")
    list(APPEND TEST_LINK_LIBS TurboMedia::Crypto)
  elseif(TEST_NAME STREQUAL "test_hls_public")
    list(APPEND TEST_LINK_LIBS TurboMedia::Player)
  elseif(TEST_NAME STREQUAL "test_hls_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Player TurboMedia::Streamer TurboMedia::Recognition)
  elseif(TEST_NAME STREQUAL "test_dash_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Codec TurboMedia::Streamer)
  elseif(TEST_NAME STREQUAL "test_http_flv_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Streamer)
  elseif(TEST_NAME STREQUAL "test_rtmp_metadata")
    list(APPEND TEST_LINK_LIBS TurboMedia::Streamer)
  elseif(TEST_NAME STREQUAL "test_mobile_optimizer")
    list(APPEND TEST_LINK_LIBS turbo_media_android)
  elseif(TEST_NAME STREQUAL "test_webrtc")
    list(APPEND TEST_LINK_LIBS TurboMedia::WebRTC TurboMedia::Core TurboMedia::Server
                          TurboMedia::Pipeline TurboMedia::Transport Salts::Core
                          TurboMedia::RTC)
  elseif(TEST_NAME STREQUAL "test_pipeline")
    list(APPEND TEST_LINK_LIBS TurboMedia::Pipeline TurboMedia::Server
                          TurboMedia::RTSP TurboMedia::ServerRTSP
                          TurboMedia::Transport TurboMedia::Codec
                          Salts::CHTTP Salts::Core)
    if(WIN32)
      list(APPEND TEST_LINK_LIBS ws2_32)
    endif()
  else()
    # 其他测试按组件链接，避免依赖已移除的兼容聚合库。
    list(
      APPEND
      TEST_LINK_LIBS
      TurboMedia::Core
      TurboMedia::Server
      TurboMedia::Codec
      TurboMedia::Device
      TurboMedia::Muxer
      TurboMedia::Demuxer
      TurboMedia::Transport
      TurboMedia::Streamer)
  endif()
  
  # 创建可执行目标
  set(TEST_TARGET_NAME turbo_media_${TEST_NAME})
  add_executable(${TEST_TARGET_NAME} ${TEST_SOURCE} ${TEST_HELPER_SOURCES})
  
  # 链接库
  target_link_libraries(${TEST_TARGET_NAME} PRIVATE ${TEST_LINK_LIBS})
  
  # 设置目标属性
  set_target_properties(${TEST_TARGET_NAME} PROPERTIES 
    FOLDER "turbomedia/tests"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  
  # Android 交叉编译目标由 tools/android-test.ps1 在设备上运行。
  if(NOT ANDROID)
    add_test(NAME ${TEST_TARGET_NAME} COMMAND ${TEST_TARGET_NAME})
    set_tests_properties(${TEST_TARGET_NAME} PROPERTIES
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
  endif()
  
  message(STATUS "Added test: ${TEST_TARGET_NAME}")
endforeach()

# 打印测试摘要
list(LENGTH TEST_SOURCES TEST_COUNT)
message(STATUS "Total tests configured: ${TEST_COUNT}")
