cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(TurboMediaStreamGraph VERSION 0.1.0 LANGUAGES C CXX)
  include(GNUInstallDirs)
  include(CTest)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(_TM_SG_ISOLATED TRUE)
endif()
add_library(turbo_media_streamgraph SHARED src/turbo_streamgraph.c src/turbo_frame_store.c
  src/turbo_streamgraph_fanout.c src/turbo_streamgraph_executor.c
  src/turbo_streamgraph_handoff.c src/turbo_streamgraph_owner_dispatch.c
  src/turbo_streamgraph_provider_completion.c)
target_include_directories(turbo_media_streamgraph
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../media/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(turbo_media_streamgraph PRIVATE SHARED_CXX)
if(MSVC)
  target_compile_options(turbo_media_streamgraph PRIVATE /W4 /WX /experimental:c11atomics)
else()
  target_compile_options(turbo_media_streamgraph PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
if(_TM_SG_ISOLATED)
  set_target_properties(turbo_media_streamgraph PROPERTIES EXPORT_NAME StreamGraph)
  add_library(TurboMedia::StreamGraph ALIAS turbo_media_streamgraph)
  install(TARGETS turbo_media_streamgraph EXPORT TurboMediaStreamGraphTargets
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../media/include/turbo_export.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_frame_store.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph_fanout.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph_executor.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph_handoff.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph_owner_dispatch.h
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_streamgraph_provider_completion.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT TurboMediaStreamGraphTargets FILE TurboMediaStreamGraphTargets.cmake
          NAMESPACE TurboMedia:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMediaStreamGraph)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaStreamGraphConfig.cmake"
       "include(\"\${CMAKE_CURRENT_LIST_DIR}/TurboMediaStreamGraphTargets.cmake\")\n")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaStreamGraphConfig.cmake"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMediaStreamGraph)
else()
  turbo_media_configure_component(turbo_media_streamgraph StreamGraph)
endif()
if(_TM_SG_ISOLATED OR NOT CMAKE_CROSSCOMPILING)
  add_executable(streamgraph_contract_test tests/test_contract.c)
  target_link_libraries(streamgraph_contract_test PRIVATE turbo_media_streamgraph)
  add_test(NAME streamgraph_contract COMMAND streamgraph_contract_test)
  add_executable(streamgraph_frame_store_test tests/test_frame_store.c)
  target_link_libraries(streamgraph_frame_store_test PRIVATE turbo_media_streamgraph)
  add_test(NAME streamgraph_frame_store COMMAND streamgraph_frame_store_test)
  add_executable(streamgraph_fanout_test tests/test_fanout.c)
  target_link_libraries(streamgraph_fanout_test PRIVATE turbo_media_streamgraph)
  add_test(NAME streamgraph_fanout COMMAND streamgraph_fanout_test)
  add_executable(streamgraph_executor_test tests/test_executor.c)
  target_link_libraries(streamgraph_executor_test PRIVATE turbo_media_streamgraph)
  add_test(NAME streamgraph_executor COMMAND streamgraph_executor_test)
  add_executable(streamgraph_multi_processor_test tests/test_multi_processor.c)
  target_link_libraries(streamgraph_multi_processor_test PRIVATE turbo_media_streamgraph)
  add_test(NAME streamgraph_multi_processor COMMAND streamgraph_multi_processor_test)
  find_package(Threads REQUIRED)
  add_executable(streamgraph_handoff_test tests/test_handoff.c)
  target_link_libraries(streamgraph_handoff_test PRIVATE turbo_media_streamgraph Threads::Threads)
  if(MSVC)
    target_compile_options(streamgraph_handoff_test PRIVATE /experimental:c11atomics)
  endif()
  add_test(NAME streamgraph_handoff COMMAND streamgraph_handoff_test)
  add_executable(streamgraph_owner_dispatch_test tests/test_owner_dispatch.c)
  target_link_libraries(streamgraph_owner_dispatch_test PRIVATE turbo_media_streamgraph Threads::Threads)
  add_test(NAME streamgraph_owner_dispatch COMMAND streamgraph_owner_dispatch_test)
  add_executable(streamgraph_provider_completion_test tests/test_provider_completion.c)
  target_link_libraries(streamgraph_provider_completion_test PRIVATE turbo_media_streamgraph Threads::Threads)
  if(MSVC)
    target_compile_options(streamgraph_provider_completion_test PRIVATE /experimental:c11atomics)
  endif()
  add_test(NAME streamgraph_provider_completion COMMAND streamgraph_provider_completion_test)
  # Real native dynamic libraries for test-only DSO quiescence qualification.
  # These are NOT alternate production Plugin loaders or providers.
  add_library(streamgraph_test_provider_n MODULE tests/fixtures/native_provider_dso.c)
  add_library(streamgraph_test_provider_n1 MODULE tests/fixtures/native_provider_dso.c)
  target_compile_definitions(streamgraph_test_provider_n
    PRIVATE TM_PROVIDER_GENERATION=101)
  target_compile_definitions(streamgraph_test_provider_n1
    PRIVATE TM_PROVIDER_GENERATION=102)
  add_executable(streamgraph_native_provider_dso_test tests/test_native_provider_dso.c)
  add_dependencies(streamgraph_native_provider_dso_test
    streamgraph_test_provider_n streamgraph_test_provider_n1)
  target_link_libraries(streamgraph_native_provider_dso_test PRIVATE
    turbo_media_streamgraph Threads::Threads ${CMAKE_DL_LIBS})
  if(MSVC)
    target_compile_options(streamgraph_native_provider_dso_test PRIVATE
      /experimental:c11atomics)
  endif()
  add_test(NAME streamgraph_native_provider_dso
    COMMAND streamgraph_native_provider_dso_test
      "$<TARGET_FILE:streamgraph_test_provider_n>"
      "$<TARGET_FILE:streamgraph_test_provider_n1>")
endif()

# Desktop-only, explicit opt-in: do not introduce FFmpeg into StreamGraph Core
# or change Android/mobile SDK transitive dependencies.
option(TM_STREAMGRAPH_ENABLE_FFMPEG_PROVIDER
       "Build FFmpeg MJPEG desktop Source adapter and executable gate" OFF)
if(TM_STREAMGRAPH_ENABLE_FFMPEG_PROVIDER)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TM_SG_AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
  pkg_check_modules(TM_SG_AVUTIL REQUIRED IMPORTED_TARGET libavutil)
  add_library(turbo_media_streamgraph_ffmpeg SHARED
    adapters/ffmpeg/src/turbo_streamgraph_ffmpeg.c
    adapters/ffmpeg/src/turbo_streamgraph_ffmpeg_async.c)
  target_include_directories(turbo_media_streamgraph_ffmpeg
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/ffmpeg/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../media/include>)
  target_link_libraries(turbo_media_streamgraph_ffmpeg
    PUBLIC turbo_media_streamgraph
    PRIVATE PkgConfig::TM_SG_AVCODEC PkgConfig::TM_SG_AVUTIL)
  target_compile_definitions(turbo_media_streamgraph_ffmpeg PRIVATE SHARED_CXX)
  # Installed desktop adapter must find the base StreamGraph library next to
  # itself, including when loaded from an unrelated consumer build directory.
  if(APPLE)
    set_target_properties(turbo_media_streamgraph_ffmpeg PROPERTIES
      INSTALL_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(turbo_media_streamgraph_ffmpeg PROPERTIES
      INSTALL_RPATH "$ORIGIN")
  endif()
  if(_TM_SG_ISOLATED)
    set_target_properties(turbo_media_streamgraph_ffmpeg
      PROPERTIES EXPORT_NAME StreamGraphFFmpeg)
    add_library(TurboMedia::StreamGraphFFmpeg ALIAS turbo_media_streamgraph_ffmpeg)
    install(TARGETS turbo_media_streamgraph_ffmpeg
      EXPORT TurboMediaStreamGraphFFmpegTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/adapters/ffmpeg/include/turbo_streamgraph_ffmpeg.h
      ${CMAKE_CURRENT_SOURCE_DIR}/adapters/ffmpeg/include/turbo_streamgraph_ffmpeg_async.h
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT TurboMediaStreamGraphFFmpegTargets
      FILE TurboMediaStreamGraphFFmpegTargets.cmake
      NAMESPACE TurboMedia::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMediaStreamGraphFFmpeg)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaStreamGraphFFmpegConfig.cmake"
      "include(CMakeFindDependencyMacro)\n"
      "find_dependency(TurboMediaStreamGraph CONFIG)\n"
      "include(\"\${CMAKE_CURRENT_LIST_DIR}/TurboMediaStreamGraphFFmpegTargets.cmake\")\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboMediaStreamGraphFFmpegConfig.cmake"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMediaStreamGraphFFmpeg)
  endif()
  if(MSVC)
    target_compile_options(turbo_media_streamgraph_ffmpeg PRIVATE /W4 /WX)
  else()
    target_compile_options(turbo_media_streamgraph_ffmpeg
      PRIVATE -Wall -Wextra -Werror -pedantic)
  endif()
  if(_TM_SG_ISOLATED OR NOT CMAKE_CROSSCOMPILING)
    add_executable(streamgraph_ffmpeg_decode_test
      adapters/ffmpeg/tests/test_ffmpeg_decode.c)
    target_link_libraries(streamgraph_ffmpeg_decode_test PRIVATE
      turbo_media_streamgraph_ffmpeg
      PkgConfig::TM_SG_AVCODEC PkgConfig::TM_SG_AVUTIL)
    target_compile_options(streamgraph_ffmpeg_decode_test
      PRIVATE -Wall -Wextra -Werror -pedantic)
    add_test(NAME streamgraph_ffmpeg_decode
             COMMAND streamgraph_ffmpeg_decode_test)
    add_executable(streamgraph_ffmpeg_async_test
      adapters/ffmpeg/tests/test_ffmpeg_async.c)
    target_link_libraries(streamgraph_ffmpeg_async_test PRIVATE
      turbo_media_streamgraph_ffmpeg Threads::Threads
      PkgConfig::TM_SG_AVCODEC PkgConfig::TM_SG_AVUTIL)
    target_compile_options(streamgraph_ffmpeg_async_test
      PRIVATE -Wall -Wextra -Werror -pedantic)
    add_test(NAME streamgraph_ffmpeg_async
             COMMAND streamgraph_ffmpeg_async_test)
  endif()
endif()
