cmake_minimum_required(VERSION 3.20)
project(StreamGraphInstalledSaltsComponentPlugin LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{TM_SALTS_SDK_ROOT} OR
   NOT IS_DIRECTORY "$ENV{TM_SALTS_SDK_ROOT}")
  message(FATAL_ERROR "TM_SALTS_SDK_ROOT must identify exact installed Salts 2.3.0 SDK")
endif()
if(NOT DEFINED ENV{TM_SG_SDK_ROOT} OR
   NOT IS_DIRECTORY "$ENV{TM_SG_SDK_ROOT}")
  message(FATAL_ERROR "TM_SG_SDK_ROOT must identify installed StreamGraph SDK")
endif()

# This qualification MUST reject uninstalled source trees, system fallback
# SDK versions, and ambiguous package lookup paths.
find_package(Salts 2.3.0 EXACT CONFIG REQUIRED
  PATHS "$ENV{TM_SALTS_SDK_ROOT}" NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)
find_package(TurboMediaStreamGraph CONFIG REQUIRED
  PATHS "$ENV{TM_SG_SDK_ROOT}" NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH)
foreach(_required IN ITEMS Salts::ComponentPlugin Salts::ComponentPluginABI
                           Salts::Plugin Salts::PluginABI
                           TurboMedia::StreamGraph)
  if(NOT TARGET ${_required})
    message(FATAL_ERROR "Installed SDK lacks required target: ${_required}")
  endif()
endforeach()
find_package(Threads REQUIRED)

# Each test DSO uses Salts::Plugin's genuine manifest and the exported
# ComponentProvider typed ABI. No private Salts source is compiled here.
foreach(_generation IN ITEMS 101 102)
  add_library(tm_installed_provider_${_generation} SHARED real_component_provider.c)
  target_include_directories(tm_installed_provider_${_generation} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(tm_installed_provider_${_generation}
    PRIVATE TM_TEST_DSO_GENERATION=${_generation})
  target_link_libraries(tm_installed_provider_${_generation} PRIVATE
    Salts::ComponentPluginABI Salts::PluginABI)
  if(MSVC)
    target_compile_options(tm_installed_provider_${_generation}
      PRIVATE /experimental:c11atomics /W4 /WX)
  else()
    target_compile_options(tm_installed_provider_${_generation}
      PRIVATE -Wall -Wextra -Werror)
  endif()
endforeach()

add_executable(tm_installed_salts_generation_test test_component_plugin.c)
target_include_directories(tm_installed_salts_generation_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tm_installed_salts_generation_test PRIVATE
  Salts::ComponentPlugin Salts::Plugin TurboMedia::StreamGraph Threads::Threads)
add_dependencies(tm_installed_salts_generation_test
  tm_installed_provider_101 tm_installed_provider_102)
if(MSVC)
  target_compile_options(tm_installed_salts_generation_test
    PRIVATE /experimental:c11atomics /W4 /WX)
else()
  target_compile_options(tm_installed_salts_generation_test
    PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME installed_salts_component_plugin_dso
  COMMAND tm_installed_salts_generation_test
    "$<TARGET_FILE:tm_installed_provider_101>"
    "$<TARGET_FILE:tm_installed_provider_102>")
set_tests_properties(installed_salts_component_plugin_dso PROPERTIES
  TIMEOUT 90)
