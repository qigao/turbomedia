find_path(VPX_INCLUDE_DIR NAMES vpx/vpx_encoder.h vpx/vpx_decoder.h REQUIRED)
find_path(OPENH264_INCLUDE_DIR NAMES wels/codec_api.h REQUIRED)
find_path(X265_INCLUDE_DIR NAMES x265.h REQUIRED)
find_path(LIBDE265_INCLUDE_DIR NAMES libde265/de265.h REQUIRED)

set(video_backends vpx_codec.c h264_codec.c h265_codec.c)
set(video_includes ${VPX_INCLUDE_DIR} ${OPENH264_INCLUDE_DIR}
                   ${X265_INCLUDE_DIR} ${LIBDE265_INCLUDE_DIR})
set(video_features TURBO_MEDIA_HAS_VPX TURBO_MEDIA_HAS_H264 TURBO_MEDIA_HAS_H265)

add_library(turbo_media_video_codec STATIC video_codec.c ${video_backends})
set_property(TARGET turbo_media_video_codec PROPERTY LINKER_LANGUAGE CXX)
target_link_libraries(turbo_media_video_codec PRIVATE TurboMediaCodecDeps::VideoCodec)
target_include_directories(turbo_media_video_codec PRIVATE ${video_includes})
target_compile_definitions(turbo_media_video_codec PRIVATE ${video_features})
turbo_media_install_codec(turbo_media_video_codec VideoCodec turbo_video_codec.h)

target_sources(turbo_media_codec PRIVATE ${video_backends})
target_include_directories(turbo_media_codec PRIVATE ${video_includes})
target_compile_definitions(turbo_media_codec PUBLIC ${video_features})
