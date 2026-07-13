# Network Transport Layer

target_sources(turbo_media PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/transport_coronet.c)

# HTTP Transport (using TurboHTTP)
if(TARGET TurboHTTP::http_client)
  target_sources(turbo_media PRIVATE ${CMAKE_CURRENT_LIST_DIR}/transport_http.c)
  target_link_libraries(turbo_media PUBLIC TurboHTTP::http_client)
endif()

# 链接 CoroNet
if(TARGET TurboNet::CoroNet)
  find_package(Threads QUIET)
  target_link_libraries(turbo_media PUBLIC TurboNet::CoroNet TurboNet::Parser)
else()
  # 尝试 find_package
  find_package(Threads QUIET)
  find_package(TurboNet QUIET)
  if(TurboNet_FOUND)
    target_link_libraries(turbo_media PUBLIC TurboNet::CoroNet TurboNet::Parser)
  endif()
endif()

# Include directory
target_include_directories(turbo_media PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:include/turbomedia/network>)
