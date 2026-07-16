# 查找测试框架依赖
if(NOT TARGET TurboUtils::TinyTest)
  find_package(TurboUtils CONFIG REQUIRED HINTS ${TURBO_MEDIA_TURBOUTILS_HINTS})
endif()

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
  set(TEST_LINK_LIBS TurboUtils::TinyTest)
  
  if(TEST_NAME MATCHES "rtsp")
    # RTSP 相关测试
    if(NOT TURBO_MEDIA_ENABLE_RTSP)
      continue()  # 跳过未启用的 RTSP 测试
    endif()
    list(APPEND TEST_LINK_LIBS TurboMedia::RTSP TurboMedia::Core TurboMedia::Server)
    if(TEST_NAME STREQUAL "test_rtsp")
      find_package(OpenSSL REQUIRED)
      list(APPEND TEST_LINK_LIBS OpenSSL::Crypto)
    endif()
  elseif(TEST_NAME STREQUAL "test_crypto")
    list(APPEND TEST_LINK_LIBS TurboMedia::Crypto)
  elseif(TEST_NAME STREQUAL "test_mobile_optimizer")
    list(APPEND TEST_LINK_LIBS turbo_media_android)
  elseif(TEST_NAME STREQUAL "test_webrtc")
    if(NOT TURBO_MEDIA_ENABLE_WEBRTC)
      continue()
    endif()
    list(APPEND TEST_LINK_LIBS TurboMedia::WebRTC TurboMedia::Core TurboMedia::Server)
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
  if(ANDROID AND NOT BUILD_TESTING)
    add_executable(${TEST_TARGET_NAME} EXCLUDE_FROM_ALL ${TEST_SOURCE} ${TEST_HELPER_SOURCES})
  else()
    add_executable(${TEST_TARGET_NAME} ${TEST_SOURCE} ${TEST_HELPER_SOURCES})
  endif()
  
  # 链接库
  target_link_libraries(${TEST_TARGET_NAME} PRIVATE ${TEST_LINK_LIBS})
  
  # 设置目标属性
  set_target_properties(${TEST_TARGET_NAME} PROPERTIES 
    FOLDER "turbomedia/tests"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  
  # Android 交叉编译目标由 tools/android-test.ps1 在设备上运行。
  if(NOT ANDROID OR BUILD_TESTING)
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
