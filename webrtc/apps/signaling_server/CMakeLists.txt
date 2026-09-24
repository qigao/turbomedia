# Source files
set(SIGNALING_SERVER_SOURCES
  src/main.c
  src/config.c
  src/server.c
  ../../src/signaling/signaling_source_identity.c
)

set(SIGNALING_SERVER_HEADERS
  include/signaling_server/config.h
  include/signaling_server/server.h
)

# Create executable
add_executable(signaling_server
  ${SIGNALING_SERVER_SOURCES}
  ${SIGNALING_SERVER_HEADERS}
)

# Include directories
target_include_directories(signaling_server
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/signaling
)

# Link libraries
target_link_libraries(signaling_server
  PRIVATE
    ${SIGNALING_SERVER_LIBS}
    TurboMedia::WebRTCSignaling
    turbo_media_auth
    Salts::Core
    Salts::TomlParser
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
  DESTINATION etc/turbomedia
  COMPONENT SignalingServer
)

# Add to IDE folder
set_target_properties(signaling_server
  PROPERTIES
    FOLDER "Applications"
)

message(STATUS "Signaling Server: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Signaling Server Output: ${CMAKE_BINARY_DIR}/bin/signaling_server")
