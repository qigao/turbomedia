cmake_minimum_required(VERSION 3.20)
project(TurboMediaCNetOwnerSDKContract VERSION 2.3 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

if(NOT DEFINED ENV{SALTS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT must be the exact installed Salts 2.3 SDK")
endif()
# No TurboMedia parent graph, build-tree SDK, old API alias, or source fallback.
find_package(Salts 2.3 CONFIG REQUIRED
             PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::CNet OR NOT TARGET Salts::Core)
  message(FATAL_ERROR "Installed Salts 2.3 must export Salts::CNet and Salts::Core")
endif()

add_executable(turbomedia_cnet_owner_sdk_smoke owner_smoke.c)
target_link_libraries(turbomedia_cnet_owner_sdk_smoke PRIVATE Salts::CNet Salts::Core)
enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME turbomedia_cnet_owner_sdk_smoke COMMAND turbomedia_cnet_owner_sdk_smoke)
endif()
