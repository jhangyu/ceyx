# heif.cmake - HEIC/HEIF decode route (libheif + libde265, DYNAMICALLY linked).
#
# The dist is produced by native/scripts/fetch_heif_deps.sh; see
# native/third_party/heif-dist/PROVENANCE.md for versions, hashes, the exact
# decode-only configure flags and the LGPL-3 relinking rationale.
#
# Included (not add_subdirectory'd) from native/CMakeLists.txt, after
# cmake/ffi.cmake, so dng_decoder_native already exists as a target.
if(NOT DNG_HOST_GENERATORS_ONLY)

if(DNG_ENABLE_HEIF)
    set(HEIF_DIST_DIR ${THIRD_PARTY_DIR}/heif-dist)

    find_path(HEIF_INCLUDE_DIR
        NAMES libheif/heif.h
        HINTS "${HEIF_DIST_DIR}/include"
        NO_DEFAULT_PATH)
    find_library(HEIF_LIBRARY
        NAMES heif
        HINTS "${HEIF_DIST_DIR}/lib"
        NO_DEFAULT_PATH)
    find_library(DE265_LIBRARY
        NAMES de265
        HINTS "${HEIF_DIST_DIR}/lib"
        NO_DEFAULT_PATH)

    # NO_DEFAULT_PATH on all three is deliberate: silently linking a Homebrew
    # libheif would stamp an absolute /opt/homebrew load command into the
    # shipped dylib, which App-Sandboxed hosts cannot open — the exact failure
    # third_party.cmake's static-libjpeg note records for libjpeg in 2026-08-17.
    if(NOT HEIF_INCLUDE_DIR OR NOT HEIF_LIBRARY OR NOT DE265_LIBRARY)
        message(FATAL_ERROR
            "HEIF decode is enabled but the vendored dist is missing.\n"
            "  expected under ${HEIF_DIST_DIR}\n"
            "  heif.h    = '${HEIF_INCLUDE_DIR}'\n"
            "  libheif   = '${HEIF_LIBRARY}'\n"
            "  libde265  = '${DE265_LIBRARY}'\n"
            "Run native/scripts/fetch_heif_deps.sh, or configure with "
            "-DDNG_ENABLE_HEIF=OFF to build without the HEIC route.")
    endif()

    target_include_directories(dng_decoder_native PRIVATE ${HEIF_INCLUDE_DIR})
    target_link_libraries(dng_decoder_native ${HEIF_LIBRARY} ${DE265_LIBRARY})
    target_compile_definitions(dng_decoder_native PRIVATE DNG_ENABLE_HEIF=1)

    message(STATUS "HEIF: libheif ${HEIF_LIBRARY} + libde265 ${DE265_LIBRARY} (dynamic)")

    if(APPLE)
        # Stage the two dylibs NEXT TO the built decoder library. Two reasons,
        # both load-bearing:
        #  1. dng_decoder_native records them as @rpath/libheif.1.dylib and
        #     @rpath/libde265.0.dylib (their install names, set by the fetch
        #     script), and the dylib carries an @loader_path rpath, so a bare
        #     dlopen out of the build directory resolves them.
        #  2. scripts/build_apps.py Phase 1 copies every sibling *.dylib from
        #     the build dir into plugin/macos/Libraries/, which is how they
        #     reach the app bundle. Staging here is what makes that free.
        # copy_if_different, so an unchanged dist does not retrigger the
        # downstream Flutter build every time.
        add_custom_command(TARGET dng_decoder_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libheif.1.dylib"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libheif.1.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libde265.0.dylib"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libde265.0.dylib"
            COMMENT "Staging libheif/libde265 next to dng_decoder_native")
    endif()
else()
    target_compile_definitions(dng_decoder_native PRIVATE DNG_ENABLE_HEIF=0)
    message(STATUS "HEIF: disabled (DNG_ENABLE_HEIF=OFF) — no heif_ symbols will be exported")
endif()

endif() # NOT DNG_HOST_GENERATORS_ONLY
