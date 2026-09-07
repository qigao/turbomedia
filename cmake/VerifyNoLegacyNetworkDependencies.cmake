if(NOT DEFINED TURBO_MEDIA_SOURCE_DIR OR
   "${TURBO_MEDIA_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TURBO_MEDIA_SOURCE_DIR is required")
endif()

file(REAL_PATH "${TURBO_MEDIA_SOURCE_DIR}" _source_dir)

set(_source_roots
    cmake
    codec
    common
    core
    crypto
    demuxer
    examples
    media
    muxer
    network
    pipeline
    player
    presets
    recognition
    server
    speech
    streamer
    tests
    tools
    transport
    vcpkg-overlays
    webrtc)

set(_source_extensions
    c cc cpp cxx
    h hh hpp hxx
    cmake json toml md
    java kt m mm swift
    ps1 sh yml yaml)

set(_candidate_files
    "${_source_dir}/CMakeLists.txt"
    "${_source_dir}/CMakePresets.json"
    "${_source_dir}/CMakeUserPresets.json"
    "${_source_dir}/README.md"
    "${_source_dir}/ARCHITECTURE.md"
    "${_source_dir}/QUICKSTART.md"
    "${_source_dir}/vcpkg-configuration.json"
    "${_source_dir}/vcpkg.json")

foreach(_root IN LISTS _source_roots)
  if(NOT IS_DIRECTORY "${_source_dir}/${_root}")
    continue()
  endif()

  foreach(_extension IN LISTS _source_extensions)
    file(GLOB_RECURSE _matches LIST_DIRECTORIES FALSE
         "${_source_dir}/${_root}/*.${_extension}")
    list(APPEND _candidate_files ${_matches})
  endforeach()
  file(GLOB_RECURSE _cmake_lists LIST_DIRECTORIES FALSE
       "${_source_dir}/${_root}/CMakeLists.txt")
  list(APPEND _candidate_files ${_cmake_lists})
endforeach()

list(REMOVE_DUPLICATES _candidate_files)
list(SORT _candidate_files)

# Assemble retired product names so this verifier does not match its own
# source text. These patterns intentionally target dependency/API identities,
# not user data such as RTSP extension tokens or public mobile package names.
set(_turbo "turbo")
set(_net "net")
set(_http "http")
set(_utils "utils")
set(_parser "parser")
set(_coro "coro")
set(_iris "iris")

set(_forbidden_patterns
    "${_turbo}${_utils}::"
    "${_turbo}${_parser}::"
    "${_turbo}${_http}"
    "${_turbo}${_net}::"
    "${_turbo}${_net}_root"
    "find_package[ \t\r\n]*\\([ \t\r\n]*${_turbo}${_net}"
    "${_coro}${_net}\\.h"
    "flowmq_${_coro}${_net}"
    "(^|[^a-z0-9_])${_coro}_(context|socket|loop|tcp|udp|tls|ws|kcp|run|sleep|spawn|event)"
    "${_iris}_(http|websocket|router)"
    "${_iris}::")

set(_violating_files)
set(_violation_count 0)

foreach(_file IN LISTS _candidate_files)
  if(NOT EXISTS "${_file}")
    continue()
  endif()

  # Archived decision records must preserve the names they migrated away from;
  # current code, configuration, tests and user-facing documentation may not.
  file(RELATIVE_PATH _relative_file "${_source_dir}" "${_file}")
  if(_relative_file MATCHES "^docs/superpowers/(plans|specs)/")
    continue()
  endif()

  file(READ "${_file}" _content)
  string(TOLOWER "${_content}" _content_lower)
  set(_file_count 0)

  foreach(_pattern IN LISTS _forbidden_patterns)
    string(REGEX MATCHALL "${_pattern}" _matches "${_content_lower}")
    list(LENGTH _matches _match_count)
    math(EXPR _file_count "${_file_count} + ${_match_count}")
  endforeach()

  if(_file_count GREATER 0)
    list(APPEND _violating_files "${_relative_file} (${_file_count})")
    math(EXPR _violation_count "${_violation_count} + ${_file_count}")
  endif()
endforeach()

if(_violating_files)
  list(JOIN _violating_files "\n  - " _violation_report)
  message(FATAL_ERROR
          "Found ${_violation_count} retired network dependency/API references:\n"
          "  - ${_violation_report}\n"
          "Migrate production and test callers to Salts CHTTP/CNet before "
          "removing this gate failure.")
endif()

message(STATUS "No retired network dependency/API references found")
