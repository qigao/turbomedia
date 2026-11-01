add_library(
  turbo_media_codec SHARED
  codec_registry.c
  g711_codec.c
  opus_codec.c
  vpx_codec.c
  h264_codec.c
  h265_codec.c)
# x265 is a static C++ library exposed through a C API.  Keep the public codec
# implementation in C, but use the C++ linker driver so its runtime and vector
# math dependencies are recorded in the shared object's dynamic dependency set.
set_property(TARGET turbo_media_codec PROPERTY LINKER_LANGUAGE CXX)
turbo_media_configure_component(turbo_media_codec Codec)
target_include_directories(
  turbo_media_codec
  PRIVATE ${OPUS_INCLUDE_DIR}
          ${VPX_INCLUDE_DIR}
          ${OPENH264_INCLUDE_DIR}
          ${X265_INCLUDE_DIR}
          ${LIBDE265_INCLUDE_DIR})
target_link_libraries(
  turbo_media_codec
  PRIVATE ${OPUS_LIBRARY}
          ${VPX_LIBRARY}
          ${OPENH264_LIBRARY}
          ${X265_LIBRARY}
          ${LIBDE265_LIBRARY})
target_compile_definitions(
  turbo_media_codec
  PUBLIC TURBO_MEDIA_HAS_OPUS
         TURBO_MEDIA_HAS_VPX
         TURBO_MEDIA_HAS_H264
         TURBO_MEDIA_HAS_H265)


# Real production Codec Provider, opt-in via TurboMedia::StreamGraphH264.
# Uses the existing OpenH264 Codec and StreamGraph, no new scheduler/decoder.
add_library(turbo_media_streamgraph_h264 SHARED
  ${PROJECT_SOURCE_DIR}/streamgraph/adapters/h264/src/turbo_streamgraph_h264.c)
turbo_media_configure_component(turbo_media_streamgraph_h264 StreamGraphH264)
target_include_directories(turbo_media_streamgraph_h264 PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/streamgraph/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/streamgraph/adapters/h264/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(turbo_media_streamgraph_h264 PUBLIC
  TurboMedia::Codec TurboMedia::StreamGraph)
install(FILES
  ${PROJECT_SOURCE_DIR}/streamgraph/adapters/h264/include/turbo_streamgraph_h264.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
