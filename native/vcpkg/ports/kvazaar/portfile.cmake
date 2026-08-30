# kvazaar 2.3.1 — NEW overlay port. No upstream vcpkg port exists at any version
# (Spec_build_rewrite.md §3.2, reason `absent`, verified 2026-08-30: 404 on
# ports/kvazaar/vcpkg.json).
#
# ACQUISITION — deliberately the GIT TAG ARCHIVE, not the release asset.
# manifest.toml [component.kvazaar.source.windows] records why: the v2.3.1
# release tarball omits src/threadwrapper/src/pthread.cpp, which kvazaar's
# CMakeLists.txt unconditionally adds to the target when WIN32 is true. The
# file is present in the tagged git tree. vcpkg_from_github fetches the tag
# archive (== git tree), so this port is correct on Windows by construction and
# is a strict superset of the release asset on macOS/Linux.
# Do NOT "simplify" this to the release tarball (handoff §B, K24).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ultravideo/kvazaar
    REF "v${VERSION}"
    SHA512 d6e18568a3bbdb96560d29df42ae603d5ade2733cc095394d5d3e5560b8e3999333b94082389e250e7352732d191f2acd45b337850183fb2a31c88e2d4e8a098
    HEAD_REF master
)

# Layer 6 / N7: kvazaar pins cmake_minimum_required(3.12), below the 3.15 that
# introduced CMP0091, so CMAKE_MSVC_RUNTIME_LIBRARY is SILENTLY IGNORED without
# this. vcpkg's own toolchain additionally writes /MT straight into
# CMAKE_C_FLAGS_RELEASE, so this is belt-and-braces here rather than the single
# point of failure it is in the shell script — keep it anyway (N8: applying the
# policy uniformly is deliberate; a future bump could re-open the hole).
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DBUILD_TESTS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    MAYBE_UNUSED_VARIABLES
        CMAKE_POSITION_INDEPENDENT_CODE
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()

# The `kvazaar` CLI encoder is not a deliverable of this project's dist
# (manifest.toml declares only the static library as an output). Removing it
# also avoids vcpkg's "executable in bin/ of a static build" policy failure.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)
foreach(_exe_dir "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
    if(EXISTS "${_exe_dir}")
        file(GLOB _kvz_tools "${_exe_dir}/kvazaar${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
        if(_kvz_tools)
            file(REMOVE ${_kvz_tools})
        endif()
        file(GLOB _remaining "${_exe_dir}/*")
        if(_remaining STREQUAL "")
            file(REMOVE_RECURSE "${_exe_dir}")
        endif()
    endif()
endforeach()

# N9 / layer 3, the consumer half. kvazaar.h declares its API
# __declspec(dllimport) unless the CONSUMER defines KVZ_STATIC_LIB, and kvazaar
# ships no CMake package config, so the usage requirement cannot propagate.
# vcpkg CAN carry it: a usage file tells any consumer what to define, and the
# libheif overlay port passes it explicitly on its own compile line.
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

# kvazaar's licence file is LICENSE, not COPYING (unlike libheif/libde265).
# LICENSE.EXT.greatest covers the vendored `greatest` test framework and is
# included because manifest.toml's licence_files glob ["COPYING*", "LICENSE*"]
# would collect it too — the LGPL/BSD source-availability obligation is
# discharged by shipping what the glob names, not a subset of it.
vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/LICENSE.EXT.greatest"
)
