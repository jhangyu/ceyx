# jxl.cmake - JPEG XL encode + decode route (2026-08-30 codec expansion).
#
# STATIC, unlike the heif dist. Three reasons, in order of weight:
#   1. libjxl is BSD-3-Clause: there is no LGPL relink duty pushing us toward a
#      separate shared library, which is the ONLY reason the heif dist is
#      dynamic (heif-dist/PROVENANCE.md:17-23).
#   2. third_party.cmake's 2026-08-17 App-Sandbox rule: every extra dylib is
#      another @rpath entry to stage, codesign and bundle on macOS, and another
#      DLL that must sit beside the decoder on Windows.
#   3. libjxl drags in highway AND brotli, so a dynamic choice would mean three
#      new shipped binaries per platform instead of zero.
#
# Absent dist is a SOFT degradation (message(WARNING), not FATAL_ERROR),
# matching CEYX_ENABLE_WEBP (cmake/encode.cmake:16-18) and deliberately unlike
# DNG_ENABLE_HEIF's hard gate (cmake/heif.cmake:62-82): src/jxl_codec.cpp
# compiles with CEYX_ENABLE_JXL=0 into entries that return the "unsupported"
# code, and the symbols stay exported so a Dart lookup still succeeds and the
# caller gets a defined error instead of a crash.
#
# Included (not add_subdirectory'd) from native/CMakeLists.txt after
# cmake/encode.cmake, so dng_decoder_native already exists as a target.
if(NOT DNG_HOST_GENERATORS_ONLY)

option(CEYX_ENABLE_JXL
       "Build the JPEG XL encode/decode route (vendored static libjxl dist)" ON)

set(CEYX_JXL_ENABLED 0)
if(CEYX_ENABLE_JXL)
    # Arch-suffixed dists first, then the historical unsuffixed path -- the
    # macOS CI builds arm64 natively and cross-compiles x86_64 on the SAME
    # runner (macos_build.yml:83-101), and a shared tree must not have one arch
    # clobber the other (fetch_heif_deps.sh:38-48).
    set(JXL_DIST_HINTS "${THIRD_PARTY_DIR}/libjxl-dist")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(JXL_DIST_HINTS "")
        foreach(_jxl_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND JXL_DIST_HINTS "${THIRD_PARTY_DIR}/libjxl-dist-${_jxl_arch}")
        endforeach()
        list(APPEND JXL_DIST_HINTS "${THIRD_PARTY_DIR}/libjxl-dist")
    elseif(WIN32)
        # Committed dist (Task 6), for the same reason as heif-dist-windows:
        # no machine in this project can produce Windows binaries locally. It
        # is the ONLY hint -- letting the search fall through to a Mach-O dist
        # left behind by a developer would surface as a link error naming the
        # decoder rather than naming this mismatch.
        set(JXL_DIST_HINTS "${THIRD_PARTY_DIR}/libjxl-dist-windows")
    endif()

    set(JXL_INCLUDE_HINTS "")
    set(JXL_LIB_HINTS "")
    foreach(_jxl_hint IN LISTS JXL_DIST_HINTS)
        list(APPEND JXL_INCLUDE_HINTS "${_jxl_hint}/include")
        list(APPEND JXL_LIB_HINTS "${_jxl_hint}/lib")
    endforeach()

    # NO_DEFAULT_PATH throughout: a system libjxl must never be picked up
    # silently, for the App-Sandbox reason cmake/heif.cmake:58-61 records.
    find_path(CEYX_JXL_INCLUDE_DIR NAMES jxl/encode.h
              HINTS ${JXL_INCLUDE_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_JXL_LIBRARY         NAMES jxl         HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_JXL_THREADS_LIBRARY NAMES jxl_threads HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_HWY_LIBRARY         NAMES hwy         HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_BROTLIDEC_LIBRARY   NAMES brotlidec   HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_BROTLIENC_LIBRARY   NAMES brotlienc   HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)
    find_library(CEYX_BROTLICOMMON_LIBRARY NAMES brotlicommon HINTS ${JXL_LIB_HINTS} NO_DEFAULT_PATH)

    if(CEYX_JXL_INCLUDE_DIR AND CEYX_JXL_LIBRARY AND CEYX_JXL_THREADS_LIBRARY
       AND CEYX_HWY_LIBRARY AND CEYX_BROTLIDEC_LIBRARY AND CEYX_BROTLIENC_LIBRARY
       AND CEYX_BROTLICOMMON_LIBRARY)
        target_include_directories(dng_decoder_native PRIVATE ${CEYX_JXL_INCLUDE_DIR})
        # Order matters for static archives on ld: jxl before its dependencies,
        # brotlicommon last because both brotli halves reference it.
        target_link_libraries(dng_decoder_native
            ${CEYX_JXL_LIBRARY}
            ${CEYX_JXL_THREADS_LIBRARY}
            ${CEYX_HWY_LIBRARY}
            ${CEYX_BROTLIENC_LIBRARY}
            ${CEYX_BROTLIDEC_LIBRARY}
            ${CEYX_BROTLICOMMON_LIBRARY})
        set(CEYX_JXL_ENABLED 1)
        message(STATUS "JXL: static ${CEYX_JXL_LIBRARY}")
    else()
        message(WARNING
            "JPEG XL disabled -- vendored dist not found.\n"
            "  searched under: ${JXL_DIST_HINTS}\n"
            "  jxl/encode.h  = '${CEYX_JXL_INCLUDE_DIR}'\n"
            "  libjxl        = '${CEYX_JXL_LIBRARY}'\n"
            "  libjxl_threads= '${CEYX_JXL_THREADS_LIBRARY}'\n"
            "  libhwy        = '${CEYX_HWY_LIBRARY}'\n"
            "  brotli        = '${CEYX_BROTLIDEC_LIBRARY}' '${CEYX_BROTLIENC_LIBRARY}' '${CEYX_BROTLICOMMON_LIBRARY}'\n"
            "Run native/scripts/fetch_libjxl_dist.sh. Every other codec is "
            "unaffected; the JXL entries will return the unsupported code.")
    endif()
else()
    message(STATUS "JXL: disabled (CEYX_ENABLE_JXL=OFF)")
endif()

target_compile_definitions(dng_decoder_native PRIVATE
    CEYX_ENABLE_JXL=${CEYX_JXL_ENABLED})

endif() # NOT DNG_HOST_GENERATORS_ONLY
