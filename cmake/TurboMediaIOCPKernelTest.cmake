# M15d / Salts #1168: independent Windows-only REAL GQCS receive gate.
# Original IOCP source is copied privately with test-only after-kernel hook;
# production RTSP continues to use the exact published Salts CNet DLL.
set(TM_M15D_IOCP_KERNEL_SOURCE_ROOT "" CACHE PATH
    "Exact Salts #1176 private Windows IOCP source checkout")
if(TM_M15D_IOCP_KERNEL_SOURCE_ROOT)
  if(NOT WIN32 OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "M15d RTSP private IOCP gate requires native Windows host")
  endif()
  set(_tm_iocp_root "${TM_M15D_IOCP_KERNEL_SOURCE_ROOT}/native-io")
  foreach(_tm_io_src IN ITEMS
      src/native_io.c src/native_io_accept_escrow.c src/native_io_iocp.c
      src/native_io_internal.h include/salts/native_io.h)
    if(NOT EXISTS "${_tm_iocp_root}/${_tm_io_src}")
      message(FATAL_ERROR "Exact private IOCP source missing: ${_tm_io_src}")
    endif()
  endforeach()
  file(STRINGS "${_tm_iocp_root}/src/native_io_iocp.c"
    _tm_iocp_hook REGEX "SALTS_NATIVE_IO_KERNEL_TEST_HOOK")
  if(NOT _tm_iocp_hook)
    message(FATAL_ERROR "Salts #1168 GQCS source hook absent; no fallback")
  endif()
  add_executable(turbo_media_rtsp_nativeio_iocp_dispatch
    "${PROJECT_SOURCE_DIR}/tests/rtsp_nativeio_iocp_dispatch.c"
    "${_tm_iocp_root}/src/native_io.c"
    "${_tm_iocp_root}/src/native_io_accept_escrow.c"
    "${_tm_iocp_root}/src/native_io_iocp.c")
  target_include_directories(turbo_media_rtsp_nativeio_iocp_dispatch PRIVATE
    "${_tm_iocp_root}/include" "${_tm_iocp_root}/src")
  target_compile_definitions(turbo_media_rtsp_nativeio_iocp_dispatch PRIVATE
    SALTS_NATIVE_IO_KERNEL_TEST_HOOK=1
    WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0600)
  target_link_libraries(turbo_media_rtsp_nativeio_iocp_dispatch PRIVATE
    TurboMedia::RTSP Salts::Platform Salts::Coroutine ws2_32)
  if(MSVC)
    target_compile_options(turbo_media_rtsp_nativeio_iocp_dispatch PRIVATE
      /W4 /experimental:c11atomics)
  endif()
  set_target_properties(turbo_media_rtsp_nativeio_iocp_dispatch PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME turbo_media_rtsp_nativeio_iocp_dispatch
    COMMAND turbo_media_rtsp_nativeio_iocp_dispatch)
  set_tests_properties(turbo_media_rtsp_nativeio_iocp_dispatch PROPERTIES
    TIMEOUT 90 RESOURCE_LOCK tm_rtsp_kernel_iocp_port)
endif()
