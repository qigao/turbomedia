foreach(_TurboMedia_ffmpeg_component IN ITEMS avformat avcodec avutil swresample
                                              swscale)
  set(_TurboMedia_ffmpeg_variable
      "FFMPEG_lib${_TurboMedia_ffmpeg_component}_LIBRARY")
  if(NOT DEFINED ${_TurboMedia_ffmpeg_variable}
     OR "${${_TurboMedia_ffmpeg_variable}}" STREQUAL "")
    message(
      FATAL_ERROR
        "Required FFmpeg component is missing: ${_TurboMedia_ffmpeg_component}")
  endif()

  if(NOT TARGET TurboMediaFFmpeg::${_TurboMedia_ffmpeg_component})
    add_library(TurboMediaFFmpeg::${_TurboMedia_ffmpeg_component} INTERFACE
                IMPORTED)
    set_target_properties(
      TurboMediaFFmpeg::${_TurboMedia_ffmpeg_component}
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}")
    target_link_libraries(
      TurboMediaFFmpeg::${_TurboMedia_ffmpeg_component}
      INTERFACE ${${_TurboMedia_ffmpeg_variable}})
  endif()
endforeach()

unset(_TurboMedia_ffmpeg_component)
unset(_TurboMedia_ffmpeg_variable)
