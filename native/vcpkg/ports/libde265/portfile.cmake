# libde265 1.1.1 — CEYX OVERLAY FORK of upstream vcpkg ports/libde265.
#
# Spec_build_rewrite.md §3.2 "Why libde265 still splits by platform" lists three
# Windows blockers in the upstream port. This fork clears all three:
#
#   (1) Upstream calls vcpkg_copy_tools(TOOL_NAMES dec265 AUTO_CLEAN) and passes
#       only -DENABLE_SDL=OFF, leaving ENABLE_DECODER at its upstream default ON,
#       which gates the dec265 tool subdirectory. dec265's bundled getopt clone
#       does not compile under clang's stricter C rules. -> we pass
#       -DENABLE_DECODER=OFF on Windows and never call vcpkg_copy_tools.
#       NOTE (transcribed from build_heif_dist_windows.sh:293 and manifest.toml
#       [component.libde265.cmake.windows]): ENABLE_DECODER gates ONLY the CLI
#       tool subdirectory, NOT the decode library. Turning it off does not
#       produce a decoder-less libde265.
#   (2) Upstream passes no clang-cl target-feature flag; libde265's CMakeLists
#       reads if(MSVC) as "cl.exe accepts all intrinsics" (layer 2). ->
#       /clang:-msse4.1 is supplied by the overlay triplet's VCPKG_C_FLAGS /
#       VCPKG_CXX_FLAGS, which APPEND to the toolchain's own flags (so pitfall
#       N16's "flags replace defaults" hazard does not arise here).
#   (3) dynamic library + static CRT has no built-in triplet. ->
#       triplets/x64-windows-heif.cmake.
#
# ACQUISITION: the exact release asset pinned in native/deps/manifest.toml
# (sha256 fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219 ==
# the sha512 below, same bytes), NOT the git tag archive upstream's port uses.
# Both upstream patches were verified to apply cleanly to this tarball.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/strukturag/libde265/releases/download/v${VERSION}/libde265-${VERSION}.tar.gz"
    FILENAME "libde265-${VERSION}.tar.gz"
    SHA512 7ecc2fc2d20bc85f2a117c16562e1abd6ad9ec92785f65dfb15a5c7038687b64ae885ad31d31beaf2fa5471b4ce4dbfee98191db9457b2a6b14eabda8c483cad
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        fix-linkage.patch
        fix-api-visibility.patch
)

# Blocker (1). On macOS/Linux the upstream default (tool built) is harmless, but
# manifest.toml's cmake.base declares ENABLE_DECODER=ON there and OFF on Windows
# only, so reproduce exactly that split rather than unifying it.
if(VCPKG_TARGET_IS_WINDOWS)
    set(_ceyx_enable_decoder OFF)
else()
    set(_ceyx_enable_decoder ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DENABLE_DECODER=${_ceyx_enable_decoder}
        -DENABLE_ENCODER=OFF
        -DENABLE_SDL=OFF
        -DENABLE_SHERLOCK265=OFF
        -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF
        -DWITH_FUZZERS=OFF
    MAYBE_UNUSED_VARIABLES
        ENABLE_SHERLOCK265
        ENABLE_INTERNAL_DEVELOPMENT_TOOLS
        WITH_FUZZERS
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libde265)
vcpkg_fixup_pkgconfig()

# Blocker (1), second half. Upstream calls vcpkg_copy_tools UNCONDITIONALLY,
# which is what forces dec265 to be built at all. Here it runs only when the
# tool actually exists (i.e. not on Windows) — vcpkg_copy_tools is still needed
# in that case to move dec265 out of bin/, or vcpkg's post-build policy check
# fails on an executable sitting in bin/. The tool is not a dist deliverable
# (manifest.toml declares only shared_lib/import_lib outputs), so it is dropped
# immediately afterwards.
if(_ceyx_enable_decoder)
    vcpkg_copy_tools(TOOL_NAMES dec265 AUTO_CLEAN)
    foreach(_tooldir "${CURRENT_PACKAGES_DIR}/tools" "${CURRENT_PACKAGES_DIR}/debug/tools")
        file(REMOVE_RECURSE "${_tooldir}")
    endforeach()
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "defined(LIBDE265_STATIC_BUILD)" "1")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "defined(LIBDE265_STATIC_BUILD)" "0")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
