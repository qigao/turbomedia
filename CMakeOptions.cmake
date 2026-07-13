include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# Address Sanitizer - only enabled for Debug builds
cmake_dependent_option(
    ENABLE_ASAN "Enable Address Sanitizer" ON
    "CMAKE_BUILD_TYPE STREQUAL Debug" OFF
)

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_MOBILE "Configure mobile platform media targets" OFF)

# Codec options
option(TURBO_MEDIA_ENABLE_OPUS "Build Opus codec support" OFF)
option(TURBO_MEDIA_ENABLE_VPX "Build VP8/VP9 codec support with libvpx" OFF)
option(TURBO_MEDIA_ENABLE_H264 "Build H.264 codec support with OpenH264" OFF)
option(TURBO_MEDIA_ENABLE_H265 "Build H.265 codec support with x265 and libde265" OFF)

# Container/Muxer options
option(TURBO_MEDIA_ENABLE_FLV "Build FLV muxer/demuxer support" OFF)
option(TURBO_MEDIA_ENABLE_MP4 "Build MP4 muxer/demuxer support" OFF)
option(TURBO_MEDIA_ENABLE_MKV "Build MKV/WebM muxer/demuxer support" OFF)
option(TURBO_MEDIA_ENABLE_MPEG "Build MPEG-TS/PS muxer/demuxer support" OFF)

# Streaming protocol options
option(TURBO_MEDIA_ENABLE_HLS "Build HLS packager support" OFF)
option(TURBO_MEDIA_ENABLE_DASH "Build DASH packager support" OFF)
option(TURBO_MEDIA_ENABLE_RTMP "Build RTMP client support with CoroNet" OFF)
option(TURBO_MEDIA_ENABLE_HTTP_FLV "Build HTTP-FLV streaming support" OFF)

# Other options
option(TURBO_MEDIA_ENABLE_FFMPEG "Build FFmpeg-backed universal player support" OFF)
option(TURBO_MEDIA_ENABLE_RTSP "Build RTSP control-plane support with CoroNet, re2c and lemon" OFF)
option(TURBO_MEDIA_ENABLE_WEBRTC
       "Build WebRTC SDP, DataChannel, and ServerRuntime bridge support" OFF)
option(TURBO_MEDIA_ENABLE_LEGACY_RTSP_TRANSPORT "Build legacy transport/rtsp sources" OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
