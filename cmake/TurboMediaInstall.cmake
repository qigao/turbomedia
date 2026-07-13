include(CMakePackageConfigHelpers)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboMediaConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/TurboMediaConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")

write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/TurboMediaConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion)

if(TARGET turbo_media_rtsp)
  install(
    TARGETS turbo_media_rtsp
    EXPORT TurboMediaTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(TURBO_MEDIA_COMPONENT_TARGETS
    turbo_media_core
    turbo_media_server
    turbo_media_codec
    turbo_media_muxer
    turbo_media_demuxer
    turbo_media_transport
    turbo_media_streamer
    turbo_media_device
    turbo_media_player
    turbo_media_sdp
    turbo_media_datachannel
    turbo_media_webrtc
    turbo_media_webrtc_signaling
    turbo_media_rtc)

foreach(TURBO_MEDIA_COMPONENT_TARGET IN LISTS TURBO_MEDIA_COMPONENT_TARGETS)
  if(TARGET ${TURBO_MEDIA_COMPONENT_TARGET})
    install(
      TARGETS ${TURBO_MEDIA_COMPONENT_TARGET}
      EXPORT TurboMediaTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endforeach()

export(
  EXPORT TurboMediaTargets
  FILE "${CMAKE_BINARY_DIR}/TurboMediaTargets.cmake"
  NAMESPACE TurboMedia::)

install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_capture.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_playback.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_codec.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_player.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_rtsp.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/media/include/turbo_export.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/core/include/turbo_media_source.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/server/include/turbo_media_server.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/muxer/include/turbo_muxer.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/demuxer/include/turbo_demuxer.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/network/include/turbo_transport.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/streamer/include/turbo_streamer.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(TARGET turbo_media_webrtc)
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_media_webrtc.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_media_webrtc_backend.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/ice_integration.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(TARGET turbo_media_webrtc_signaling)
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/webrtc_signaling.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/http_api.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(TARGET turbo_media_datachannel)
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_datachannel.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_datachannel_errors.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_dc_msg.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_sdp.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/include/turbo_srtp_defs.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(TARGET turbo_media_rtsp)
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/media/rtsp/parser/turbo_rtsp_parser.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/media/rtsp/parser/turbo_rtsp_message.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/media/rtsp/parser/turbo_rtsp_rtp.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/media/rtsp/parser/turbo_rtsp_sdp.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(
  FILES "${CMAKE_BINARY_DIR}/TurboMediaConfig.cmake"
        "${CMAKE_BINARY_DIR}/TurboMediaConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")

install(
  EXPORT TurboMediaTargets
  FILE TurboMediaTargets.cmake
  NAMESPACE TurboMedia::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia")
