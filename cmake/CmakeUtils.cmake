# TurboMedia CMake utilities

function(cmake_config_target target_name)
    set(options)
    set(oneValueArgs FOLDER VERSION SOVERSION EXPORT_NAME ALIAS OUTPUT_NAME)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_target_property(target_type ${target_name} TYPE)
    if(NOT target_type)
        message(FATAL_ERROR "cmake_config_target: target '${target_name}' does not exist")
    endif()

    if(ARG_ALIAS)
        if(target_type STREQUAL "EXECUTABLE")
            add_executable(${ARG_ALIAS} ALIAS ${target_name})
        else()
            add_library(${ARG_ALIAS} ALIAS ${target_name})
        endif()
    endif()

    if(ARG_FOLDER)
        set_target_properties(${target_name} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()

    if(ARG_EXPORT_NAME)
        set_target_properties(${target_name} PROPERTIES EXPORT_NAME ${ARG_EXPORT_NAME})
    endif()

    if(ARG_OUTPUT_NAME)
        set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()

    if(target_type STREQUAL "SHARED_LIBRARY" OR target_type STREQUAL "STATIC_LIBRARY")
        if(NOT ARG_VERSION AND PROJECT_VERSION)
            set(ARG_VERSION ${PROJECT_VERSION})
        endif()
        if(NOT ARG_SOVERSION AND PROJECT_VERSION_MAJOR)
            set(ARG_SOVERSION ${PROJECT_VERSION_MAJOR})
        endif()
        
        if(ARG_VERSION)
          set_target_properties(${target_name} PROPERTIES VERSION ${ARG_VERSION})
        endif()
        if(ARG_SOVERSION)
          set_target_properties(${target_name} PROPERTIES SOVERSION ${ARG_SOVERSION})
        endif()
    endif()

endfunction()

function(turbo_media_configure_component target_name export_name)
  cmake_parse_arguments(ARG "STANDALONE;NO_MEDIA_HEADERS;EXPLICIT_EXPORTS" "" "" ${ARGN})
  cmake_config_target(
    ${target_name}
    ALIAS TurboMedia::${export_name}
    EXPORT_NAME ${export_name}
    FOLDER "turbomedia/components")
  if(NOT ARG_NO_MEDIA_HEADERS)
    target_include_directories(${target_name}
      PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/media/codec/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  endif()
  get_target_property(_turbo_media_target_type ${target_name} TYPE)
  if(NOT ARG_EXPLICIT_EXPORTS AND
     (_turbo_media_target_type STREQUAL "SHARED_LIBRARY" OR
      _turbo_media_target_type STREQUAL "MODULE_LIBRARY"))
    target_compile_definitions(${target_name} PRIVATE SHARED_CXX)
  endif()
  unset(_turbo_media_target_type)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /wd4100 /wd4996)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  if(ARG_STANDALONE)
    install(TARGETS ${target_name} EXPORT TurboMedia${export_name}Targets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${export_name}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${export_name}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${export_name})
    install(EXPORT TurboMedia${export_name}Targets NAMESPACE TurboMedia::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboMedia COMPONENT ${export_name})
  else()
    install(
      TARGETS ${target_name}
      EXPORT TurboMediaTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endfunction()

function(cmake_add_grammar TARGET_NAME)
  set(options LEXER_DEPENDS_ON_GRAMMAR)
  set(oneValueArgs LEXER_RE GRAMMAR_Y FOLDER LEXER_OUTPUT)
  set(multiValueArgs LEXER_DEPENDS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  string(TOLOWER "${TARGET_NAME}" target_name_lower)

  if(ARG_LEXER_RE)
    get_filename_component(ARG_LEXER_RE "${ARG_LEXER_RE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(ARG_GRAMMAR_Y)
    get_filename_component(ARG_GRAMMAR_Y "${ARG_GRAMMAR_Y}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  set(absolute_lexer_depends)
  foreach(lexer_depend IN LISTS ARG_LEXER_DEPENDS)
    get_filename_component(lexer_depend "${lexer_depend}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND absolute_lexer_depends "${lexer_depend}")
  endforeach()

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_H "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.h")
  endif()

  if(ARG_LEXER_RE)
    if(ARG_LEXER_OUTPUT)
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${ARG_LEXER_OUTPUT}")
    else()
      set(LEXER_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_lexer_gen.c")
    endif()
    set(lexer_depends ${ARG_LEXER_RE} ${absolute_lexer_depends})
    if(ARG_LEXER_DEPENDS_ON_GRAMMAR)
      if(NOT ARG_GRAMMAR_Y)
        message(FATAL_ERROR "cmake_add_grammar: LEXER_DEPENDS_ON_GRAMMAR requires GRAMMAR_Y")
      endif()
      list(APPEND lexer_depends ${GRAMMAR_H})
    endif()
    add_custom_command(
      OUTPUT ${LEXER_GEN}
      COMMAND ${RE2C_EXECUTABLE} -o ${LEXER_GEN} ${ARG_LEXER_RE}
      DEPENDS ${lexer_depends} "${RE2C_EXECUTABLE}"
      COMMENT "Generating ${TARGET_NAME} lexer with re2c"
      VERBATIM)
    set(LEXER_TARGET "${TARGET_NAME}_lexer_codegen")
    add_custom_target(${LEXER_TARGET} DEPENDS ${LEXER_GEN})
    if(ARG_FOLDER)
      set_target_properties(${LEXER_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_LEXER_GEN ${LEXER_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_LEXER_TARGET ${LEXER_TARGET} PARENT_SCOPE)
  endif()

  if(ARG_GRAMMAR_Y)
    set(GRAMMAR_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.c")
    set(GRAMMAR_Y_GEN "${CMAKE_CURRENT_BINARY_DIR}/${target_name_lower}_grammar_gen.y")
    add_custom_command(
      OUTPUT ${GRAMMAR_GEN} ${GRAMMAR_H}
      COMMAND ${CMAKE_COMMAND} -E copy ${ARG_GRAMMAR_Y} ${GRAMMAR_Y_GEN}
      COMMAND "${LEMON_EXECUTABLE}" "-T${LEMPAR}" "${GRAMMAR_Y_GEN}"
      DEPENDS ${ARG_GRAMMAR_Y} "${LEMON_EXECUTABLE}" "${LEMPAR}"
      COMMENT "Generating ${TARGET_NAME} parser with lemon"
      VERBATIM)
    set(GRAMMAR_TARGET "${TARGET_NAME}_grammar_codegen")
    add_custom_target(${GRAMMAR_TARGET} DEPENDS ${GRAMMAR_GEN} ${GRAMMAR_H})
    if(ARG_FOLDER)
      set_target_properties(${GRAMMAR_TARGET} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
    set(${TARGET_NAME}_GRAMMAR_GEN ${GRAMMAR_GEN} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_H ${GRAMMAR_H} PARENT_SCOPE)
    set(${TARGET_NAME}_GRAMMAR_TARGET ${GRAMMAR_TARGET} PARENT_SCOPE)
  endif()
endfunction()

function(cmake_add_source VAR)
  set(options RECURSE)
  set(oneValueArgs)
  set(multiValueArgs DIRS EXCLUDES PATTERNS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(glob_mode GLOB)
  if(ARG_RECURSE)
    set(glob_mode GLOB_RECURSE)
  endif()

  if(NOT ARG_DIRS)
    set(ARG_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
  endif()

  if(NOT ARG_PATTERNS)
    set(ARG_PATTERNS "*.c" "*.cpp" "*.h" "*.hpp" "*.cc" "*.hh")
  endif()

  set(patterns)
  foreach(dir ${ARG_DIRS})
    foreach(pat ${ARG_PATTERNS})
      list(APPEND patterns "${dir}/${pat}")
    endforeach()
  endforeach()

  file(${glob_mode} collected ${patterns})

  if(ARG_EXCLUDES)
    list(REMOVE_ITEM collected ${ARG_EXCLUDES})
  endif()

  set(${VAR} ${collected} PARENT_SCOPE)
endfunction()

function(cmake_add_executable target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(TARGET ${target_name})
    message(FATAL_ERROR "cmake_add_executable: target '${target_name}' already exists")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cmake_add_executable: SOURCES is required for '${target_name}'")
  endif()

  add_executable(${target_name} ${ARG_SOURCES})
  if(ARG_LIBS)
    target_link_libraries(${target_name} PRIVATE ${ARG_LIBS})
  endif()
  if(ARG_DEFS)
    target_compile_definitions(${target_name} PRIVATE ${ARG_DEFS})
  endif()
  if(ARG_INCLUDES)
    target_include_directories(${target_name} PRIVATE ${ARG_INCLUDES})
  endif()
  cmake_config_target(${target_name} FOLDER "${ARG_FOLDER}")
endfunction()

function(cmake_add_test target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(TARGET ${target_name})
    message(FATAL_ERROR "cmake_add_test: target '${target_name}' already exists")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cmake_add_test: SOURCES is required for '${target_name}'")
  endif()

  add_executable(${target_name} ${ARG_SOURCES})
  if(ARG_LIBS)
    target_link_libraries(${target_name} PRIVATE ${ARG_LIBS})
  endif()
  if(ARG_DEFS)
    target_compile_definitions(${target_name} PRIVATE ${ARG_DEFS})
  endif()
  if(ARG_INCLUDES)
    target_include_directories(${target_name} PRIVATE ${ARG_INCLUDES})
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  cmake_config_target(${target_name} FOLDER "${ARG_FOLDER}")
endfunction()

function(cmake_add_benchmark target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(TARGET ${target_name})
    message(FATAL_ERROR "cmake_add_benchmark: target '${target_name}' already exists")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cmake_add_benchmark: SOURCES is required for '${target_name}'")
  endif()

  add_executable(${target_name} ${ARG_SOURCES})
  if(ARG_LIBS)
    target_link_libraries(${target_name} PRIVATE ${ARG_LIBS})
  endif()
  if(ARG_DEFS)
    target_compile_definitions(${target_name} PRIVATE ${ARG_DEFS})
  endif()
  if(ARG_INCLUDES)
    target_include_directories(${target_name} PRIVATE ${ARG_INCLUDES})
  endif()
  cmake_config_target(${target_name} FOLDER "${ARG_FOLDER}")
endfunction()

function(turbo_media_add_module_test target source)
  cmake_add_executable(${target} SOURCES ${source}
    LIBS Salts::TinyTest ${ARGN} FOLDER "turbomedia/tests")
  if(UNIX)
    target_link_libraries(${target} PRIVATE m)
  endif()
  # Android test executables run on-device.
  if(NOT ANDROID)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  endif()
endfunction()
