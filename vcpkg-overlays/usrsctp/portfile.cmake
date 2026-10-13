vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sctplab/usrsctp
    # Experimental source-only candidate for #222, not an SDK release.
    # CI #38340383818 independently verified this exact upstream tarball.
    REF b6b96d387db78550b6997625b051744e8e7949ce
    SHA512 f01944555f07f26c9035780dd333b8f1a750bd237cbbb90506098ae418f458d23b175938440bff78d872cc104c9107914f97ced89c8e40c3140492a54eddf874
    PATCHES
        fix_export.patch
        mingw.diff
)

if(VCPKG_TARGET_IS_IOS)
    set(_usrsctp_recv_thread "${SOURCE_PATH}/usrsctplib/user_recv_thread.c")
    vcpkg_replace_string(
        "${_usrsctp_recv_thread}"
        "#if defined(INET) || defined(INET6)\n#include <sys/types.h>"
        "#include <TargetConditionals.h>\n#if TARGET_OS_IPHONE\n#define __APPLE_USE_RFC_3542 1\n#endif\n\n#if defined(INET) || defined(INET6)\n#include <sys/types.h>"
    )
    vcpkg_replace_string(
        "${_usrsctp_recv_thread}"
        "#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__)"
        "#if (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(__DragonFly__) || defined(__FreeBSD__)"
        IGNORE_UNCHANGED
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -Dsctp_werror=OFF
        -Dsctp_build_programs=OFF
)

vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

configure_file("${SOURCE_PATH}/LICENSE.md" "${CURRENT_PACKAGES_DIR}/share/usrsctp/copyright" COPYONLY)

vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-${PORT} CONFIG_PATH share/unofficial-${PORT})
