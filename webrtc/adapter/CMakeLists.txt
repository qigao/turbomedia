add_library(turbo_media_rtc SHARED)
turbo_media_configure_component(turbo_media_rtc RTC)
turbo_media_add_component_alias(turbo_media_rtc TurboMedia::RTC)

target_include_directories(
  turbo_media_rtc
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rtc/include>
         $<INSTALL_INTERFACE:include>)

target_sources(
  turbo_media_rtc
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtc/rtc_adapter.c
          ${CMAKE_CURRENT_SOURCE_DIR}/rtc/rtc_session.c)
target_link_libraries(turbo_media_rtc PUBLIC TurboMedia::Core TurboMedia::Server)
target_compile_definitions(turbo_media_rtc PUBLIC TURBO_MEDIA_HAS_RTC)

if(TURBO_MEDIA_RTC_BACKEND_TURBORTC)
  set(TURBORTC_ROOT "" CACHE PATH "Installed TurboRTC package prefix")
  set(_turbomedia_turbortc_hints)
  if(TURBORTC_ROOT)
    list(APPEND _turbomedia_turbortc_hints "${TURBORTC_ROOT}")
  endif()

  find_package(TurboRTC CONFIG REQUIRED HINTS ${_turbomedia_turbortc_hints})
  if(NOT TARGET TurboRTC::MediaEngine)
    message(FATAL_ERROR "TurboRTC package does not export TurboRTC::MediaEngine")
  endif()
  if(TARGET TurboRTC::Media)
    get_target_property(_turbomedia_legacy_rtc_media_location
                        TurboRTC::Media IMPORTED_LOCATION_RELEASE)
    if(_turbomedia_legacy_rtc_media_location MATCHES "[/\\\\]turbo_media\\.dll$")
      message(FATAL_ERROR
              "The selected TurboRTC package still depends on the removed turbo_media.dll. "
              "Rebuild and install C:/projects/cpp/turbonet/turbo-webrtc against TurboMedia::Device.")
    endif()
  endif()

  target_sources(
    turbo_media_rtc
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtc/rtc_turbortc_backend.c)
  target_link_libraries(turbo_media_rtc PRIVATE TurboRTC::MediaEngine)
  target_compile_definitions(turbo_media_rtc PUBLIC TURBO_MEDIA_HAS_TURBORTC_BACKEND)

  unset(_turbomedia_legacy_rtc_media_location)
  unset(_turbomedia_turbortc_hints)
endif()
