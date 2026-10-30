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

  set(DESKTOP_SERVICE_TESTS
      test_dash_local test_hls_local test_http_flv_local test_pipeline
      test_rtmp_metadata test_server_rtsp_adapter test_server_runtime test_webrtc)
  if(NOT TURBO_MEDIA_HAS_DESKTOP_SERVICES AND
     TEST_NAME IN_LIST DESKTOP_SERVICE_TESTS)
    continue()
  endif()
  
  # 根据测试类型确定链接库
  set(TEST_LINK_LIBS Salts::TinyTest)
  if(UNIX)
    list(APPEND TEST_LINK_LIBS m)
  endif()
  
  if(TEST_NAME STREQUAL "test_server_rtsp_adapter")
    list(APPEND TEST_LINK_LIBS TurboMedia::RTSP TurboMedia::Core
                               TurboMedia::ServerRTSP)
  elseif(TEST_NAME MATCHES "rtsp")
    list(APPEND TEST_LINK_LIBS TurboMedia::RTSP TurboMedia::Core)
    if(TEST_NAME STREQUAL "test_rtsp")
      list(APPEND TEST_LINK_LIBS OpenSSL::Crypto)
    endif()
  elseif(TEST_NAME STREQUAL "test_client_processing")
    list(APPEND TEST_LINK_LIBS TurboMedia::ClientProcessing
                               TurboMediaFFmpeg::avformat)
  elseif(TEST_NAME STREQUAL "test_rtc_client" OR
         TEST_NAME STREQUAL "test_rtc_subscriber" OR
         TEST_NAME STREQUAL "test_rtc_playback_sink" OR
         TEST_NAME STREQUAL "test_rtc_capture_source")
    list(APPEND TEST_LINK_LIBS TurboMedia::RTCClient)
  elseif(TEST_NAME STREQUAL "test_benchmark")
    list(APPEND TEST_LINK_LIBS TurboMedia::Codec Salts::Capture Salts::Playback)
  elseif(TEST_NAME STREQUAL "test_capture")
    list(APPEND TEST_LINK_LIBS Salts::Capture)
  elseif(TEST_NAME STREQUAL "test_codec")
    list(APPEND TEST_LINK_LIBS TurboMedia::Codec)
  elseif(TEST_NAME STREQUAL "test_encoder")
    # Tests decode their actual PNG/JPEG/WebP outputs using the one selected
    # FFmpeg package, never a local fake codec or external CLI.
    list(APPEND TEST_LINK_LIBS TurboMedia::Encoder
                               TurboMediaFFmpeg::avcodec
                               TurboMediaFFmpeg::avutil)
  elseif(TEST_NAME STREQUAL "test_containers")
    list(APPEND TEST_LINK_LIBS TurboMedia::Muxer TurboMedia::Demuxer)
  elseif(TEST_NAME STREQUAL "test_integration")
    list(APPEND TEST_LINK_LIBS TurboMedia::Codec Salts::Capture Salts::Playback)
  elseif(TEST_NAME STREQUAL "test_crypto")
    list(APPEND TEST_LINK_LIBS TurboMedia::Crypto)
  elseif(TEST_NAME STREQUAL "test_hls_public")
    list(APPEND TEST_LINK_LIBS TurboMedia::Player)
  elseif(TEST_NAME STREQUAL "test_hls_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Player TurboMedia::Streamer TurboMedia::Recognition)
  elseif(TEST_NAME STREQUAL "test_player_audio")
    list(APPEND TEST_LINK_LIBS TurboMedia::Player)
  elseif(TEST_NAME STREQUAL "test_dash_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Codec TurboMedia::Streamer)
  elseif(TEST_NAME STREQUAL "test_http_flv_local")
    list(APPEND TEST_LINK_LIBS TurboMedia::Streamer CHttp::Server)
  elseif(TEST_NAME STREQUAL "test_rtmp_metadata")
    list(APPEND TEST_LINK_LIBS TurboMedia::Streamer)
  elseif(TEST_NAME STREQUAL "test_mobile_optimizer")
    list(APPEND TEST_LINK_LIBS turbo_media_android)
  elseif(TEST_NAME STREQUAL "test_android_capture")
    list(APPEND TEST_LINK_LIBS Salts::Capture Salts::Platform)
  elseif(TEST_NAME STREQUAL "test_media_source")
    list(APPEND TEST_LINK_LIBS TurboMedia::Core)
  elseif(TEST_NAME STREQUAL "test_webrtc")
    list(APPEND TEST_LINK_LIBS TurboMedia::WebRTC TurboMedia::Core TurboMedia::Server
                          TurboMedia::Pipeline TurboMedia::Transport Salts::Core
                          TurboMedia::RTC)
  elseif(TEST_NAME STREQUAL "test_pipeline")
    list(APPEND TEST_LINK_LIBS TurboMedia::Pipeline TurboMedia::Server
                          TurboMedia::RTSP TurboMedia::ServerRTSP
                          TurboMedia::Transport TurboMedia::Codec
                          CHttp::Client CHttp::Server Salts::Core)
    if(WIN32)
      list(APPEND TEST_LINK_LIBS ws2_32)
    endif()
  elseif(TEST_NAME STREQUAL "test_server_runtime")
    list(APPEND TEST_LINK_LIBS TurboMedia::Server)
  elseif(TEST_NAME MATCHES "transport")
    list(APPEND TEST_LINK_LIBS TurboMedia::Transport CHttp::Client CHttp::Server)
  else()
    list(APPEND TEST_LINK_LIBS TurboMedia::Core)
  endif()
  
  # 创建可执行目标
  set(TEST_TARGET_NAME turbo_media_${TEST_NAME})
  add_executable(${TEST_TARGET_NAME} ${TEST_SOURCE} ${TEST_HELPER_SOURCES})
  
  # 链接库
  target_link_libraries(${TEST_TARGET_NAME} PRIVATE ${TEST_LINK_LIBS})
  if(TEST_NAME STREQUAL "test_encoder")
    target_include_directories(${TEST_TARGET_NAME} PRIVATE ${STB_INCLUDE_DIR})
  endif()
  
  # M15d: desktop-only real CNet RTSP/RFC2250 MPEG-2 RTP -> bounded
  # FFmpeg delayed Codec conformance. Compile the EXACT existing optional
  # adapter source into the test executable; no second production Provider,
  # decoder, scheduler or mobile SDK dependency is introduced. The deployed
  # FFmpeg addon remains independently installed and qualified on #187.
  if(TEST_NAME STREQUAL "test_rtsp" AND NOT ANDROID)
    target_sources(${TEST_TARGET_NAME} PRIVATE
      ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/src/turbo_streamgraph_ffmpeg_delayed.c)
    target_include_directories(${TEST_TARGET_NAME} PRIVATE
      ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/include
      ${PROJECT_SOURCE_DIR}/streamgraph/include)
    target_compile_definitions(${TEST_TARGET_NAME} PRIVATE
      TM_TEST_RTSP_MPEG2_CODEC_GATE=1)
    target_link_libraries(${TEST_TARGET_NAME} PRIVATE
      TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avutil)
  endif()
  
  # Actual, installed-Salts CMeta/ComponentPlugin DSO lifecycle gate,
  # Linux-only until the joint cross-platform RTSP+FFmpeg+DLOpen profile
  # has its own independent installed SDK conformance. The production
  # RTSP and optional tm_ffd Sources are UNMODIFIED; the canonical
  # Salts Plugin loader and ComponentPlugin Scope own both generations.
  if(TEST_NAME STREQUAL "test_rtsp" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(_tm_generation IN ITEMS 101 102)
      add_library(tm_rtsp_component_provider_${_tm_generation} SHARED
        ${PROJECT_SOURCE_DIR}/streamgraph/tests/installed_salts/real_component_provider.c
        ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/src/turbo_streamgraph_ffmpeg.c
        ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/src/turbo_streamgraph_ffmpeg_async.c
        ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/src/turbo_streamgraph_ffmpeg_delayed.c)
      target_include_directories(tm_rtsp_component_provider_${_tm_generation} PRIVATE
        ${PROJECT_SOURCE_DIR}/streamgraph/tests/installed_salts
        ${PROJECT_SOURCE_DIR}/streamgraph/adapters/ffmpeg/include
        ${PROJECT_SOURCE_DIR}/streamgraph/include
        ${PROJECT_SOURCE_DIR}/media/include)
      target_compile_definitions(tm_rtsp_component_provider_${_tm_generation}
        PRIVATE TM_TEST_DSO_GENERATION=${_tm_generation}
                TM_TEST_FFMPEG_DSO=1 SHARED_CXX)
      target_link_libraries(tm_rtsp_component_provider_${_tm_generation} PRIVATE
        Salts::ComponentPluginABI Salts::PluginABI Salts::CNet
        turbo_media_streamgraph
        TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avutil)
      target_compile_options(tm_rtsp_component_provider_${_tm_generation} PRIVATE
        -Wall -Wextra -Werror -Wno-unused-function)
      set_target_properties(tm_rtsp_component_provider_${_tm_generation}
        PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
      add_dependencies(${TEST_TARGET_NAME}
        tm_rtsp_component_provider_${_tm_generation})
    endforeach()
    target_include_directories(${TEST_TARGET_NAME} PRIVATE
      ${PROJECT_SOURCE_DIR}/streamgraph/tests/installed_salts)
    target_compile_definitions(${TEST_TARGET_NAME} PRIVATE
      TM_TEST_RTSP_MPEG2_PROVIDER_DSO=1 TM_TEST_FFMPEG_DSO=1)
    target_link_libraries(${TEST_TARGET_NAME} PRIVATE
      Salts::ComponentPlugin Salts::Plugin Salts::CNet
      turbo_media_streamgraph Threads::Threads)
  endif()

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
    if(TEST_NAME STREQUAL "test_rtsp" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # The original and fault-injection tests use the SAME two real
      # installed-Salts ComponentPlugin DSOs and fixed localhost RTSP port.
      # Prevent simultaneous CTest invocations from competing for them.
      set(_rtsp_dso_fixture_env
        "TM_RTSP_TEST_DSO_101=$<TARGET_FILE:tm_rtsp_component_provider_101>;TM_RTSP_TEST_DSO_102=$<TARGET_FILE:tm_rtsp_component_provider_102>")
      set_tests_properties(${TEST_TARGET_NAME} PROPERTIES
        ENVIRONMENT "${_rtsp_dso_fixture_env}"
        RESOURCE_LOCK tm_rtsp_cnet_ports)

      # Deliberately fail a NONFATAL predicate with the original NativeIO
      # callback and real Provider DSO worker both on stack. The assertion
      # is checked after callbacks join and the original Ticket is settled.
      add_test(NAME turbo_media_test_rtsp_deferred_check_cleanup
        COMMAND ${TEST_TARGET_NAME})
      set_tests_properties(turbo_media_test_rtsp_deferred_check_cleanup PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        ENVIRONMENT "${_rtsp_dso_fixture_env};TM_RTSP_INJECT_DEFERRED_CHECK=1"
        RESOURCE_LOCK tm_rtsp_cnet_ports
        TIMEOUT 120)
    endif()
    if(TEST_NAME STREQUAL "test_benchmark")
      set_tests_properties(${TEST_TARGET_NAME} PROPERTIES LABELS performance)
    endif()
  endif()
  
  message(STATUS "Added test: ${TEST_TARGET_NAME}")
endforeach()

# 打印测试摘要
list(LENGTH TEST_SOURCES TEST_COUNT)
message(STATUS "Total tests configured: ${TEST_COUNT}")

# M15d / Salts #1168: opt-in PRIVATE source-copy of original NativeIO.
# The after-real-epoll-wait hook belongs only to this test executable.
# Production TurboMedia::RTSP continues to link released Salts::CNet.
set(TM_M15D_NATIVE_IO_KERNEL_SOURCE_ROOT "" CACHE PATH
    "Exact Salts #1176 private NativeIO test source checkout (Linux only)")
if(TM_M15D_NATIVE_IO_KERNEL_SOURCE_ROOT)
  if((NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE) OR
     CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "M15d NativeIO kernel dispatch consumer is Linux epoll / macOS kqueue host-only")
  endif()
  set(_tm_native_root "${TM_M15D_NATIVE_IO_KERNEL_SOURCE_ROOT}/native-io")
  set(_tm_private_native_platform)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _tm_private_native_platform
      src/native_io_linux.c src/native_io_epoll.c src/native_io_io_uring.c)
  else()
    list(APPEND _tm_private_native_platform
      src/native_io_bsd.c src/native_io_kqueue.c)
  endif()
  foreach(_tm_kernel_source IN ITEMS
      src/native_io.c src/native_io_accept_escrow.c
      src/native_io_readiness.c src/native_io_readiness.h
      ${_tm_private_native_platform})
    if(NOT EXISTS "${_tm_native_root}/${_tm_kernel_source}")
      message(FATAL_ERROR "Exact private NativeIO source missing: ${_tm_kernel_source}")
    endif()
  endforeach()
  file(STRINGS "${_tm_native_root}/src/native_io_readiness.c"
    _tm_kernel_hook REGEX "SALTS_NATIVE_IO_KERNEL_TEST_HOOK")
  if(NOT _tm_kernel_hook)
    message(FATAL_ERROR
      "Salts #1168 real-after-epoll dispatch hook absent; no fallback")
  endif()
  add_executable(turbo_media_rtsp_nativeio_kernel_dispatch
    rtsp_nativeio_kernel_dispatch.c
    "${_tm_native_root}/src/native_io.c"
    "${_tm_native_root}/src/native_io_accept_escrow.c"
    "${_tm_native_root}/src/native_io_readiness.c")
  foreach(_tm_platform_file IN LISTS _tm_private_native_platform)
    target_sources(turbo_media_rtsp_nativeio_kernel_dispatch PRIVATE
      "${_tm_native_root}/${_tm_platform_file}")
  endforeach()
  target_include_directories(turbo_media_rtsp_nativeio_kernel_dispatch PRIVATE
    "${_tm_native_root}/include" "${_tm_native_root}/src")
  target_compile_definitions(turbo_media_rtsp_nativeio_kernel_dispatch PRIVATE
    SALTS_NATIVE_IO_KERNEL_TEST_HOOK=1)
  if(APPLE)
    target_compile_definitions(turbo_media_rtsp_nativeio_kernel_dispatch
      PRIVATE _DARWIN_C_SOURCE=1)
  endif()
  target_link_libraries(turbo_media_rtsp_nativeio_kernel_dispatch PRIVATE
    TurboMedia::RTSP Salts::Platform Salts::Coroutine Threads::Threads)
  target_compile_options(turbo_media_rtsp_nativeio_kernel_dispatch PRIVATE
    -Wall -Wextra -Werror -Wno-unused-parameter)
  set_target_properties(turbo_media_rtsp_nativeio_kernel_dispatch PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden)
  add_test(NAME turbo_media_rtsp_nativeio_kernel_dispatch
    COMMAND turbo_media_rtsp_nativeio_kernel_dispatch)
  set_tests_properties(turbo_media_rtsp_nativeio_kernel_dispatch PROPERTIES
    TIMEOUT 90 RESOURCE_LOCK tm_rtsp_kernel_port)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/TurboMediaIOCPKernelTest.cmake")
