if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.23)
  project(TurboMediaLemonHost LANGUAGES C)
endif()

if(CMAKE_CROSSCOMPILING)
  include(ExternalProject)

  set(lemon_host_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/host-build")
  if(CMAKE_HOST_WIN32)
    foreach(required_environment MSVC_ROOT WINDOWS_SDK_ROOT WINDOWS_SDK_VERSION INCLUDE LIB)
      if("$ENV{${required_environment}}" STREQUAL "")
        message(FATAL_ERROR
                "The in-tree Lemon host build requires ${required_environment} in the configure preset environment")
      endif()
    endforeach()
    set(lemon_host_executable "${lemon_host_binary_dir}/lemon.exe")
    set(lemon_host_toolchain_args
        "-DCMAKE_C_COMPILER:FILEPATH=$ENV{MSVC_ROOT}/bin/Hostx64/x64/cl.exe"
        "-DCMAKE_LINKER:FILEPATH=$ENV{MSVC_ROOT}/bin/Hostx64/x64/link.exe"
        "-DCMAKE_AR:FILEPATH=$ENV{MSVC_ROOT}/bin/Hostx64/x64/lib.exe"
        "-DCMAKE_RC_COMPILER:FILEPATH=$ENV{WINDOWS_SDK_ROOT}/bin/$ENV{WINDOWS_SDK_VERSION}/x64/rc.exe"
        "-DCMAKE_MT:FILEPATH=$ENV{WINDOWS_SDK_ROOT}/bin/$ENV{WINDOWS_SDK_VERSION}/x64/mt.exe")
  else()
    find_program(lemon_host_c_compiler NAMES cc gcc clang REQUIRED)
    set(lemon_host_executable "${lemon_host_binary_dir}/lemon")
    set(lemon_host_toolchain_args
        "-DCMAKE_C_COMPILER:FILEPATH=${lemon_host_c_compiler}")
  endif()

  ExternalProject_Add(
    turbo_media_lemon_host
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    BINARY_DIR "${lemon_host_binary_dir}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_CACHE_ARGS
      "-DCMAKE_BUILD_TYPE:STRING=Release"
      "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
      ${lemon_host_toolchain_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target lemon --config Release
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${lemon_host_executable}")

  add_executable(lemon IMPORTED GLOBAL)
  set_target_properties(lemon PROPERTIES IMPORTED_LOCATION "${lemon_host_executable}")
  add_dependencies(lemon turbo_media_lemon_host)
else()
  add_executable(lemon lemon.c)
  target_compile_features(lemon PRIVATE c_std_11)
  set_target_properties(lemon PROPERTIES FOLDER tools)
endif()

add_executable(TurboNet::Lemon ALIAS lemon)
