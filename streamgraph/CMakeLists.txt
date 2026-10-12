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
add_library(turbo_media_streamgraph SHARED src/turbo_streamgraph.c)
target_include_directories(turbo_media_streamgraph
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../media/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(turbo_media_streamgraph PRIVATE SHARED_CXX)
if(MSVC)
  target_compile_options(turbo_media_streamgraph PRIVATE /W4 /WX)
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
endif()
