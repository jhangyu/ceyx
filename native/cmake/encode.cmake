# encode.cmake - RGBA8 -> JPEG / WebP encode route (2026-08-30).
#
# JPEG reuses the libjpeg-turbo that third_party.cmake already resolves for the
# DNG SDK, so this file only has to put its headers on dng_decoder_native
# (pipeline.cmake attaches them to dng_sdk PUBLIC, which propagates, but stating
# it here keeps the encode TU independent of that detail).
#
# WebP links the STATIC dist produced by native/scripts/fetch_libwebp_dist.sh.
# Static, not dynamic like the heif dist: libwebp is BSD-3 (no LGPL relink
# duty) and third_party.cmake's App-Sandbox rule forbids an absolute
# LC_LOAD_DYLIB into a Homebrew prefix. NO_DEFAULT_PATH on both probes is what
# enforces that — a system libwebp must never be picked up silently.
#
# Absent dist is a soft degradation, not a configure failure: the JPEG path is
# the one Phase 13 depends on, and src/ffi/encode_ffi_api.cpp compiles with
# CEYX_ENABLE_WEBP=0 into ceyx_encode_webp_rgba8 returning
# kCeyxEncodeErrUnsupported (the symbol stays exported, so a Dart-side lookup
# still succeeds and the caller gets a defined error instead of a crash).
#
# Included (not add_subdirectory'd) from native/CMakeLists.txt after
# cmake/heif.cmake, so dng_decoder_native already exists as a target.
if(NOT DNG_HOST_GENERATORS_ONLY)

option(CEYX_ENABLE_WEBP
       "Build the WebP encode route (vendored static libwebp dist)" ON)

if(DNG_USE_LIBJPEG)
    target_include_directories(dng_decoder_native PRIVATE ${JPEG_INCLUDE_DIRS})
endif()

set(CEYX_WEBP_ENABLED 0)
if(CEYX_ENABLE_WEBP)
    set(WEBP_DIST_HINTS "${THIRD_PARTY_DIR}/libwebp-dist")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(WEBP_DIST_HINTS "")
        foreach(_webp_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND WEBP_DIST_HINTS "${THIRD_PARTY_DIR}/libwebp-dist-${_webp_arch}")
        endforeach()
        list(APPEND WEBP_DIST_HINTS "${THIRD_PARTY_DIR}/libwebp-dist")
    endif()
    set(WEBP_INCLUDE_HINTS "")
    set(WEBP_LIB_HINTS "")
    foreach(_webp_hint IN LISTS WEBP_DIST_HINTS)
        list(APPEND WEBP_INCLUDE_HINTS "${_webp_hint}/include")
        list(APPEND WEBP_LIB_HINTS "${_webp_hint}/lib")
    endforeach()

    find_path(CEYX_WEBP_INCLUDE_DIR
        NAMES webp/encode.h
        HINTS ${WEBP_INCLUDE_HINTS}
        NO_DEFAULT_PATH)
    find_library(CEYX_WEBP_LIBRARY
        NAMES webp
        HINTS ${WEBP_LIB_HINTS}
        NO_DEFAULT_PATH)
    # libwebp 1.6 factors its YUV conversion into a separate static archive;
    # omitting it surfaces as undefined _SharpYuvConvert at dylib link time.
    find_library(CEYX_SHARPYUV_LIBRARY
        NAMES sharpyuv
        HINTS ${WEBP_LIB_HINTS}
        NO_DEFAULT_PATH)

    if(CEYX_WEBP_INCLUDE_DIR AND CEYX_WEBP_LIBRARY AND CEYX_SHARPYUV_LIBRARY)
        target_include_directories(dng_decoder_native PRIVATE ${CEYX_WEBP_INCLUDE_DIR})
        target_link_libraries(dng_decoder_native
            ${CEYX_WEBP_LIBRARY} ${CEYX_SHARPYUV_LIBRARY})
        set(CEYX_WEBP_ENABLED 1)
        message(STATUS "WebP encode: static ${CEYX_WEBP_LIBRARY}")
    else()
        message(WARNING
            "WebP encode disabled — vendored dist not found.\n"
            "  searched under: ${WEBP_DIST_HINTS}\n"
            "  webp/encode.h = '${CEYX_WEBP_INCLUDE_DIR}'\n"
            "  libwebp       = '${CEYX_WEBP_LIBRARY}'\n"
            "  libsharpyuv   = '${CEYX_SHARPYUV_LIBRARY}'\n"
            "Run native/scripts/fetch_libwebp_dist.sh. JPEG encode is unaffected; "
            "ceyx_encode_webp_rgba8 will return kCeyxEncodeErrUnsupported.")
    endif()
else()
    message(STATUS "WebP encode: disabled (CEYX_ENABLE_WEBP=OFF)")
endif()

target_compile_definitions(dng_decoder_native PRIVATE
    CEYX_ENABLE_WEBP=${CEYX_WEBP_ENABLED})

endif() # NOT DNG_HOST_GENERATORS_ONLY
