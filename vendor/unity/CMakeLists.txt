# Unity Testing Framework CMake Configuration

# Create Unity static library
add_library(unity STATIC
    src/unity.c
)

# Set include directories
target_include_directories(unity
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Unity target properties
set_target_properties(unity PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# Enable double precision support for Unity
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# Compiler flags (Unity uses C99 features)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unity PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-variable
    )
elseif(MSVC)
    target_compile_options(unity PRIVATE
        /W4
        /wd4100  # Suppress unused parameter warnings
        /wd4127  # Suppress conditional expression is constant warnings
    )
endif()
set_target_properties(unity PROPERTIES FOLDER "vendor/unity")
