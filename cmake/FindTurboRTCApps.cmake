include(FindPackageHandleStandardArgs)

set(_TurboRTCApps_ROOT_HINTS)
if(TURBORTC_ROOT)
  list(APPEND _TurboRTCApps_ROOT_HINTS "${TURBORTC_ROOT}")
endif()
if(DEFINED ENV{TURBORTC_ROOT})
  list(APPEND _TurboRTCApps_ROOT_HINTS "$ENV{TURBORTC_ROOT}")
endif()

find_path(
  TurboRTCApps_INCLUDE_DIR
  NAMES turbo_room_service.h turbo_sfu_node.h turbo_recorder.h
  HINTS ${_TurboRTCApps_ROOT_HINTS}
  PATH_SUFFIXES include)

find_library(
  TurboRTCApps_CONFERENCE_LIBRARY
  NAMES turbo_conference
  HINTS ${_TurboRTCApps_ROOT_HINTS}
  PATH_SUFFIXES lib)

find_library(
  TurboRTCApps_RECORDER_LIBRARY
  NAMES turbo_recorder
  HINTS ${_TurboRTCApps_ROOT_HINTS}
  PATH_SUFFIXES lib)

find_package_handle_standard_args(
  TurboRTCApps
  REQUIRED_VARS TurboRTCApps_INCLUDE_DIR
                TurboRTCApps_CONFERENCE_LIBRARY
                TurboRTCApps_RECORDER_LIBRARY)

if(TurboRTCApps_FOUND)
  if(NOT TARGET TurboRTCApps::Conference)
    add_library(TurboRTCApps::Conference INTERFACE IMPORTED)
    set_target_properties(
      TurboRTCApps::Conference
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${TurboRTCApps_INCLUDE_DIR}"
                 INTERFACE_LINK_LIBRARIES "${TurboRTCApps_CONFERENCE_LIBRARY}")
  endif()

  if(NOT TARGET TurboRTCApps::Recorder)
    add_library(TurboRTCApps::Recorder INTERFACE IMPORTED)
    set_target_properties(
      TurboRTCApps::Recorder
      PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${TurboRTCApps_INCLUDE_DIR}"
                 INTERFACE_LINK_LIBRARIES "${TurboRTCApps_RECORDER_LIBRARY}")
  endif()
endif()

mark_as_advanced(
  TurboRTCApps_INCLUDE_DIR
  TurboRTCApps_CONFERENCE_LIBRARY
  TurboRTCApps_RECORDER_LIBRARY)

unset(_TurboRTCApps_ROOT_HINTS)
