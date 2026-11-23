function(turbo_media_copy_windows_runtime artifact kind destination)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
  if(TURBO_MEDIA_RUNTIME_OBJDUMP)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${TURBO_MEDIA_RUNTIME_OBJDUMP}")
  endif()
  file(GET_RUNTIME_DEPENDENCIES
    ${kind} "${artifact}"
    DIRECTORIES ${ARGN}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicting
    PRE_EXCLUDE_REGEXES "^(api|ext)-ms-")
  if(unresolved OR conflicting_FILENAMES)
    message(FATAL_ERROR
      "${artifact} has unresolved runtime dependencies [${unresolved}] or conflicting DLLs [${conflicting_FILENAMES}]")
  endif()
  file(TO_CMAKE_PATH "$ENV{SystemRoot}" windows_root)
  string(TOLOWER "${windows_root}" windows_root)
  set(application_dlls)
  foreach(dll IN LISTS resolved)
    file(TO_CMAKE_PATH "${dll}" dll_path)
    string(TOLOWER "${dll_path}" dll_path)
    cmake_path(IS_PREFIX windows_root "${dll_path}" NORMALIZE system_dll)
    if(NOT system_dll)
      list(APPEND application_dlls "${dll}")
    endif()
  endforeach()
  if(application_dlls)
    file(INSTALL DESTINATION "${destination}" TYPE SHARED_LIBRARY FILES ${application_dlls})
  endif()
endfunction()

function(turbo_media_install_windows_runtime target component)
  if(NOT WIN32)
    return()
  endif()

  get_target_property(target_type ${target} TYPE)
  if(target_type STREQUAL "SHARED_LIBRARY")
    set(kind LIBRARIES)
  else()
    set(kind EXECUTABLES)
  endif()

  # Resolve DLLs from the same SDKs, configuration and toolchain as the build.
  set(search_dirs "$<TARGET_FILE_DIR:${target}>")
  foreach(root IN ITEMS SALTS_BASE_ROOT CMETA_ROOT SALTS_CNET_ROOT SALTS_UTILS_ROOT
                        SALTSNET_ROOT CHTTP_ROOT TURBODB_ROOT)
    if(DEFINED ENV{${root}} AND IS_DIRECTORY "$ENV{${root}}/bin")
      list(APPEND search_dirs "$ENV{${root}}/bin")
    endif()
  endforeach()
  get_filename_component(codec_lib_dir "${OPUS_LIBRARY}" DIRECTORY)
  get_filename_component(codec_prefix "${codec_lib_dir}" DIRECTORY)
  if(IS_DIRECTORY "${codec_prefix}/debug/bin")
    list(APPEND search_dirs "$<$<CONFIG:Debug>:${codec_prefix}/debug/bin>")
  endif()
  if(IS_DIRECTORY "${codec_prefix}/bin")
    list(APPEND search_dirs "${codec_prefix}/bin")
  endif()
  get_filename_component(compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  list(APPEND search_dirs "${compiler_dir}")
  if(MINGW)
    set(runtime_objdump "${CMAKE_OBJDUMP}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}")
    set(runtime_destination "${CMAKE_INSTALL_BINDIR}")
  else()
    set(runtime_destination "\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
  endif()

  install(CODE "
    include([==[${CMAKE_CURRENT_FUNCTION_LIST_FILE}]==])
    set(TURBO_MEDIA_RUNTIME_OBJDUMP [==[${runtime_objdump}]==])
    turbo_media_copy_windows_runtime(
      [==[$<TARGET_FILE:${target}>]==]
      ${kind}
      \"${runtime_destination}\"
      [==[${search_dirs}]==])"
    COMPONENT ${component})
endfunction()

function(turbo_media_install_application target component)
  install(TARGETS ${target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT ${component})
  turbo_media_install_windows_runtime(${target} ${component})
endfunction()
