find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h REQUIRED)

set(audio_backends g711_codec.c opus_codec.c)
set(audio_features TURBO_MEDIA_HAS_OPUS)

add_library(turbo_media_audio_codec STATIC audio_codec.c ${audio_backends})
target_link_libraries(turbo_media_audio_codec PRIVATE TurboMediaCodecDeps::AudioCodec)
target_include_directories(turbo_media_audio_codec PRIVATE ${OPUS_INCLUDE_DIR})
target_compile_definitions(turbo_media_audio_codec PRIVATE ${audio_features})
turbo_media_install_codec(turbo_media_audio_codec AudioCodec turbo_audio_codec.h)

target_sources(turbo_media_codec PRIVATE ${audio_backends})
target_include_directories(turbo_media_codec PRIVATE ${OPUS_INCLUDE_DIR})
target_compile_definitions(turbo_media_codec PUBLIC ${audio_features})
