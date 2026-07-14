add_library(turbo_media_vendor_monocypher STATIC
    monocypher.c
    monocypher.h
)
add_library(TurboMediaVendor::Monocypher ALIAS turbo_media_vendor_monocypher)

target_include_directories(
    turbo_media_vendor_monocypher
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Disable warnings for third-party code
if(MSVC)
    target_compile_options(turbo_media_vendor_monocypher PRIVATE /W0)
else()
    target_compile_options(turbo_media_vendor_monocypher PRIVATE -w)
endif()
set_target_properties(turbo_media_vendor_monocypher PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    FOLDER "vendor"
)
