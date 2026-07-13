if(TURBO_MEDIA_ENABLE_FFMPEG)
  find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h REQUIRED)
  find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat REQUIRED)
  find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec REQUIRED)
  find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil REQUIRED)
  find_library(FFMPEG_SWRESAMPLE_LIBRARY NAMES swresample REQUIRED)
  find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale REQUIRED)
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ffmpeg_player.c)
  target_include_directories(turbo_media PRIVATE ${FFMPEG_INCLUDE_DIR})
  target_link_libraries(
    turbo_media
    PRIVATE ${FFMPEG_AVFORMAT_LIBRARY}
            ${FFMPEG_AVCODEC_LIBRARY}
            ${FFMPEG_AVUTIL_LIBRARY}
            ${FFMPEG_SWRESAMPLE_LIBRARY}
            ${FFMPEG_SWSCALE_LIBRARY})
  target_compile_definitions(turbo_media PUBLIC TURBO_MEDIA_HAS_FFMPEG)
endif()
