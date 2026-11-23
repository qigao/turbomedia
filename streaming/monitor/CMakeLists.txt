add_executable(turbo_monitor_console monitor_main.c monitor_media.c)
if(WIN32)
  if(MSVC)
    target_sources(turbo_monitor_console PRIVATE windows.manifest)
  else()
    enable_language(RC)
    set_property(SOURCE windows.rc PROPERTY OBJECT_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/windows.manifest")
    target_sources(turbo_monitor_console PRIVATE windows.rc)
  endif()
endif()
target_link_libraries(turbo_monitor_console PRIVATE
  Salts::Core CHttp::App
  TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avformat
  TurboMediaFFmpeg::avutil TurboMediaFFmpeg::swscale)
set_target_properties(turbo_monitor_console PROPERTIES FOLDER "turbomedia/streaming")
turbo_media_install_application(turbo_monitor_console MonitorConsole)
install(DIRECTORY web/ DESTINATION "${CMAKE_INSTALL_DATADIR}/TurboMedia/monitor"
        COMPONENT MonitorConsole)

find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_executable(turbo_monitor_rtsp_fixture tests/rtsp_fixture.c)
  target_link_libraries(turbo_monitor_rtsp_fixture PRIVATE
    Salts::Core TurboMedia::Codec TurboMedia::RTP TurboMedia::ServerRTSP
    TurboMediaFFmpeg::avcodec TurboMediaFFmpeg::avformat)
  set_target_properties(turbo_monitor_rtsp_fixture PROPERTIES FOLDER "turbomedia/tests")
  add_test(NAME turbo_monitor_console_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_console.py"
      $<TARGET_FILE:turbo_monitor_console> $<TARGET_FILE:turbo_monitor_rtsp_fixture>
      "${CMAKE_CURRENT_SOURCE_DIR}/web")
  set_tests_properties(turbo_monitor_console_e2e PROPERTIES TIMEOUT 65)
endif()
