cmake_minimum_required(VERSION 3.20)

# Signaling Server Application
project(
  TurboSignalingServer
  VERSION 1.0.0
  LANGUAGES C
  DESCRIPTION "Production-ready WebRTC signaling server"
)

# Source files
set(SIGNALING_SERVER_SOURCES
  src/main.c
  src/config.c
  src/server.c
)

set(SIGNALING_SERVER_HEADERS
  include/signaling_server/config.h
  include/signaling_server/server.h
)

if(ENABLE_RTC_SCXML_WORKFLOW)
  list(APPEND SIGNALING_SERVER_LIBS TurboRTC::RtcSessionWorkflow)
endif()

if(ENABLE_RTC_CCXML_WORKFLOW)
  list(APPEND SIGNALING_SERVER_SOURCES src/ccxml_adapter.c)
endif()

# Create executable
add_executable(signaling_server
  ${SIGNALING_SERVER_SOURCES}
  ${SIGNALING_SERVER_HEADERS}
)

# Include directories
target_include_directories(signaling_server
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${TURBOMEDIA_ROOT}/include
)

# Link libraries
target_link_libraries(signaling_server
  PRIVATE
    ${SIGNALING_SERVER_LIBS}
    TurboMedia::WebRTCSignaling
    TurboUtils::Core
    $<$<PLATFORM_ID:Linux>:m>
    $<$<PLATFORM_ID:Linux>:pthread>
)

# Compiler options
target_compile_options(signaling_server
  PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

# Set output directory
set_target_properties(signaling_server
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "signaling_server"
)

# Installation
install(
  TARGETS signaling_server
  RUNTIME DESTINATION bin
  COMPONENT SignalingServer
)

# Install configuration example
install(
  FILES config/signaling.toml.example
  DESTINATION etc/turbonet
  COMPONENT SignalingServer
)

# Install systemd service file (Linux only)
if(UNIX AND NOT APPLE)
  install(
    FILES systemd/signaling-server.service
    DESTINATION lib/systemd/system
    COMPONENT SignalingServer
  )
endif()

# Create package
set(CPACK_PACKAGE_NAME "TurboSignalingServer")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VENDOR "TurboNet") 

include(CPack)

# Add to IDE folder
set_target_properties(signaling_server
  PROPERTIES
    FOLDER "Applications"
)

message(STATUS "Signaling Server: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Signaling Server Output: ${CMAKE_BINARY_DIR}/bin/signaling_server")
