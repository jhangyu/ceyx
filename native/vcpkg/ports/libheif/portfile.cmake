# libheif 1.23.2 — CEYX OVERLAY FORK of upstream vcpkg ports/libheif.
#
# WHY A FORK (Spec_build_rewrite.md §3.2, reason `feature`): upstream's port
# declares one HEVC *encoder* feature, `hevc`, which maps to WITH_X265. x265 is
# GPL-2.0 and is excluded by this project by name (K3). There is no WITH_KVAZAAR
# mapping anywhere in the upstream port at any version. This fork adds a
# `kvazaar` feature and drops the x265 one entirely, so the GPL path cannot be
# selected even by accident.
#
# ACQUISITION: the exact release asset pinned in native/deps/manifest.toml
# (sha256 8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405 ==
# the sha512 below, same bytes) at version 1.23.2 — NOT upstream's 1.23.1.
# All three retained upstream patches were verified to apply cleanly to it.
# gdk-pixbuf.patch and cmake-project-include.cmake are NOT carried: they exist
# only for WITH_GDK_PIXBUF and WITH_X265, both of which are hard OFF below.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/strukturag/libheif/releases/download/v${VERSION}/libheif-${VERSION}.tar.gz"
    FILENAME "libheif-${VERSION}.tar.gz"
    SHA512 c40fb665e9e0e1b0ea1c618dc189f88afdb0738992ff3601fefd0a94d3c3ba089867f6b2cb141278462dd1c7e139f6af280356a9130281122e1f764096e170cb
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        cxx-linkage-pkgconfig.diff
        find-modules.diff
        symbol-exports.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        aom      WITH_AOM_DECODER
        aom      WITH_AOM_ENCODER
        aom      VCPKG_LOCK_FIND_PACKAGE_AOM
        kvazaar  WITH_KVAZAAR
)

vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG} "${PKGCONFIG}")

# N12: libheif's FindAOM.cmake tries find_package(AOM CONFIG) FIRST. On a macOS
# machine with `brew install aom` that resolves Homebrew's SHARED dylib and the
# vcpkg-installed static aom is never consulted — a green build with zero
# aom_codec_* symbols merged in. Reproduced from fetch_heif_deps.sh:300-312.
set(_ceyx_extra_options "")
if(VCPKG_TARGET_IS_OSX)
    list(APPEND _ceyx_extra_options "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DBUILD_TESTING=OFF
        -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
        -DPLUGIN_DIRECTORY=  # empty
        # Codecs are built INTO libheif. A dlopen'd plugin directory does not
        # survive app packaging (handoff §B).
        -DENABLE_PLUGIN_LOADING=OFF
        -DWITH_LIBDE265=ON
        -DWITH_LIBDE265_PLUGIN=OFF
        -DWITH_KVAZAAR_PLUGIN=OFF
        -DWITH_AOM_DECODER_PLUGIN=OFF
        -DWITH_AOM_ENCODER_PLUGIN=OFF
        # K3: named explicitly, never merely omitted, so an upstream default
        # flip cannot pull a GPL encoder in.
        -DWITH_X265=OFF
        -DWITH_X264=OFF
        -DWITH_UVG266=OFF
        -DWITH_VVDEC=OFF
        -DWITH_VVENC=OFF
        -DWITH_DAV1D=OFF
        -DWITH_SvtEnc=OFF
        -DWITH_RAV1E=OFF
        -DWITH_OpenH264_DECODER=OFF
        -DWITH_FFMPEG_DECODER=OFF
        -DWITH_JPEG_DECODER=OFF
        -DWITH_JPEG_ENCODER=OFF
        -DWITH_OpenJPEG_DECODER=OFF
        -DWITH_OpenJPEG_ENCODER=OFF
        -DWITH_OPENJPH_ENCODER=OFF
        -DWITH_UNCOMPRESSED_CODEC=OFF
        -DWITH_HEADER_COMPRESSION=OFF
        -DWITH_LIBSHARPYUV=OFF
        -DWITH_EXAMPLES=OFF
        -DWITH_GDK_PIXBUF=OFF
        -DWITH_REDUCED_VISIBILITY=ON
        -DWITH_FUZZERS=OFF
        -DBUILD_DOCUMENTATION=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_Brotli=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_Doxygen=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_LIBDE265=ON
        -DVCPKG_LOCK_FIND_PACKAGE_PNG=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_TIFF=OFF
        ${FEATURE_OPTIONS}
        ${_ceyx_extra_options}
    MAYBE_UNUSED_VARIABLES
        VCPKG_LOCK_FIND_PACKAGE_AOM
        VCPKG_LOCK_FIND_PACKAGE_Brotli
        BUILD_DOCUMENTATION
        CMAKE_IGNORE_PREFIX_PATH
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/libheif")
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libheif/heif_export.h" "!defined(LIBHEIF_STATIC_BUILD)" "1")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libheif/heif_export.h" "!defined(LIBHEIF_STATIC_BUILD)" "0")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/libheif" "${CURRENT_PACKAGES_DIR}/debug/lib/libheif")

file(GLOB maybe_plugins "${CURRENT_PACKAGES_DIR}/plugins/libheif/*")
if(maybe_plugins STREQUAL "")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/plugins" "${CURRENT_PACKAGES_DIR}/debug/plugins")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
