add_library(sha2 STATIC sha-256.c sha-256.h)

target_include_directories(
  sha2 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
              $<INSTALL_INTERFACE:include>)

set_target_properties(sha2 PROPERTIES FOLDER "vendor")
