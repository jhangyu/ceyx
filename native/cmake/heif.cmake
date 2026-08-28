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
    # Dist selection (2026-08-28). fetch_heif_deps.sh writes a host-architecture
    # dist to third_party/heif-dist and a cross-architecture one to
    # third_party/heif-dist-<arch>, so an x86_64 cross-build cannot clobber the
    # arm64 dist a shared working tree is using. Prefer the arch-suffixed dist
    # when this build targets a specific architecture, and fall back to the
    # historical unsuffixed path — which is what every non-cross build uses, so
    # their behaviour is unchanged.
    set(HEIF_DIST_DIR ${THIRD_PARTY_DIR}/heif-dist)
    set(HEIF_DIST_HINTS "${HEIF_DIST_DIR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(HEIF_DIST_HINTS "")
        foreach(_heif_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND HEIF_DIST_HINTS "${THIRD_PARTY_DIR}/heif-dist-${_heif_arch}")
        endforeach()
        list(APPEND HEIF_DIST_HINTS "${HEIF_DIST_DIR}")
    endif()
    set(HEIF_INCLUDE_HINTS "")
    set(HEIF_LIB_HINTS "")
    foreach(_heif_hint IN LISTS HEIF_DIST_HINTS)
        list(APPEND HEIF_INCLUDE_HINTS "${_heif_hint}/include")
        list(APPEND HEIF_LIB_HINTS "${_heif_hint}/lib")
    endforeach()

    find_path(HEIF_INCLUDE_DIR
        NAMES libheif/heif.h
        HINTS ${HEIF_INCLUDE_HINTS}
        NO_DEFAULT_PATH)
    find_library(HEIF_LIBRARY
        NAMES heif
        HINTS ${HEIF_LIB_HINTS}
        NO_DEFAULT_PATH)
    find_library(DE265_LIBRARY
        NAMES de265
        HINTS ${HEIF_LIB_HINTS}
        NO_DEFAULT_PATH)

    # NO_DEFAULT_PATH on all three is deliberate: silently linking a Homebrew
    # libheif would stamp an absolute /opt/homebrew load command into the
    # shipped dylib, which App-Sandboxed hosts cannot open — the exact failure
    # third_party.cmake's static-libjpeg note records for libjpeg in 2026-08-17.
    if(NOT HEIF_INCLUDE_DIR OR NOT HEIF_LIBRARY OR NOT DE265_LIBRARY)
        message(FATAL_ERROR
            "HEIF decode is enabled but the vendored dist is missing.\n"
            "  searched under: ${HEIF_DIST_HINTS}\n"
            "  heif.h    = '${HEIF_INCLUDE_DIR}'\n"
            "  libheif   = '${HEIF_LIBRARY}'\n"
            "  libde265  = '${DE265_LIBRARY}'\n"
            "Run native/scripts/fetch_heif_deps.sh, or configure with "
            "-DDNG_ENABLE_HEIF=OFF to build without the HEIC route.")
    endif()

    # Re-derive the dist root from what was actually found, so the POST_BUILD
    # staging below copies out of the SAME dist that was linked (they differ
    # whenever the arch-suffixed dist won the search above).
    get_filename_component(HEIF_DIST_DIR "${HEIF_LIBRARY}" DIRECTORY)
    get_filename_component(HEIF_DIST_DIR "${HEIF_DIST_DIR}" DIRECTORY)

    # Fail at configure time, with a message that names the problem, rather than
    # letting a stale wrong-architecture dist surface as an opaque linker error.
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        execute_process(COMMAND lipo -archs "${HEIF_LIBRARY}"
                        OUTPUT_VARIABLE HEIF_HAVE_ARCHS
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE HEIF_LIPO_RC)
        if(HEIF_LIPO_RC EQUAL 0)
            foreach(_heif_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
                if(NOT "${HEIF_HAVE_ARCHS}" MATCHES "(^| )${_heif_arch}( |$)")
                    message(FATAL_ERROR
                        "HEIF dist architecture mismatch.\n"
                        "  ${HEIF_LIBRARY}\n"
                        "  has archs '${HEIF_HAVE_ARCHS}', this build targets "
                        "'${CMAKE_OSX_ARCHITECTURES}'.\n"
                        "Rebuild the dist for the target architecture:\n"
                        "  DNG_HEIF_ARCH=${_heif_arch} native/scripts/fetch_heif_deps.sh\n"
                        "or configure with -DDNG_ENABLE_HEIF=OFF.")
                endif()
            endforeach()
        endif()
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
