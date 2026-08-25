# pipeline.cmake - dng_sdk and the dng_decoder_native source set
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 271-397)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
# =============================================================================
# Phase 14: dng_sdk, dng_decoder_native, and test targets are skipped when
# building only host-side generators (Stage 1 of cross-compile).
# =============================================================================
if(NOT DNG_HOST_GENERATORS_ONLY)

# Adobe DNG SDK
set(DNG_SDK_DIR ${THIRD_PARTY_DIR}/dng_sdk/source)
file(GLOB_RECURSE DNG_SOURCES "${DNG_SDK_DIR}/*.cpp")
list(REMOVE_ITEM DNG_SOURCES "${DNG_SDK_DIR}/dng_xmp_sdk.cpp" "${DNG_SDK_DIR}/dng_xmp.cpp")

# Halide-accelerated bridge implementations (Stage3 demosaic, warp, Stage4 render).
    # W6 H-3: bridge .cpp live in src/ (auto-discovered by glob for dng_decoder_native;
    # explicitly listed for standalone test targets).
    # Production/test_decode call the AOT entry directly and fail fast if GPU is unsupported.

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/pipeline/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
# Route A adoption (P16, 2026-06-12): pin the SDK render reference to IEEE stepwise.
# The Stage4 99.62 dB ±1 LSB residual is solely the default clang -ffp-contract=on
# FMA grouping in the SDK render math (P2-1 forensics). The render bottlenecks
# (RefBaseline* in dng_reference.cpp + the inline dng_1d_table::Interpolate lerp +
# matrix/spline/huesat helpers) are *inline-shared across many TUs*; per-source
# COMPILE_OPTIONS leaves a residual 98 px ±1 LSB because COMDAT folding can still
# bind a call to a contract=on copy emitted by an un-pinned TU. The robust fix that
# reproduces the P2-1 private-build (-ffp-contract=off whole libdng_sdk → byte-exact
# vs Halide strict, 0 diff) is a library-wide option. Stage1/2/3 outputs quantize to
# uint16 and are empirically unaffected (PSNR 999 + Stage3 SHA unchanged — see
# Task_P16_routeA_adoption.md). The Halide Metal strict_float Stage4 kernel is the
# matching GPU side; together Stage4-isolated is byte-exact.
# W3 (2026-08-21, Windows port, risk R3): the flag above is load-bearing, not a
# style choice, so the Windows toolchain is *chosen* by it rather than the other
# way round. clang-cl accepts the clang driver spelling when it is routed
# through /clang:; MSVC's cl.exe has no equivalent (/fp:strict is a different,
# stronger-but-not-identical guarantee and has never been validated against the
# byte-exact Stage4 reference), so cl.exe is rejected at configure time instead
# of silently producing a different colour pipeline. Override deliberately with
# -DDNG_ALLOW_MSVC_FP_CONTRACT=ON if you are re-baselining the colour gate.
option(DNG_ALLOW_MSVC_FP_CONTRACT
       "Allow building with MSVC cl.exe despite the missing -ffp-contract=off equivalent (colour byte-exactness is then UNVERIFIED)"
       OFF)
if(MSVC AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dng_sdk PRIVATE /clang:-ffp-contract=off)
    elseif(DNG_ALLOW_MSVC_FP_CONTRACT)
        message(WARNING "Building dng_sdk with cl.exe: -ffp-contract=off has no "
                        "equivalent, Stage4 colour byte-exactness is UNVERIFIED.")
        target_compile_options(dng_sdk PRIVATE /fp:strict)
    else()
        message(FATAL_ERROR
            "dng_sdk requires -ffp-contract=off (see the Route A note above); "
            "cl.exe does not support it. Configure with clang-cl, e.g. the "
            "'windows-vulkan' preset (-DCMAKE_CXX_COMPILER=clang-cl), or set "
            "-DDNG_ALLOW_MSVC_FP_CONTRACT=ON to accept an unverified colour path.")
    endif()
else()
    target_compile_options(dng_sdk PRIVATE -ffp-contract=off)
endif()
target_include_directories(dng_sdk PUBLIC ${DNG_SDK_DIR} ${INC_DIR})
if(DNG_USE_LIBJPEG)
    target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=1)
else()
    target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=0)
endif()
if(ANDROID)
    target_compile_definitions(dng_sdk PUBLIC qDNG64Bit=1)
endif()
if(DNG_USE_LIBJPEG)
    target_include_directories(dng_sdk PUBLIC ${JPEG_INCLUDE_DIRS})
    target_link_libraries(dng_sdk ZLIB::ZLIB ${JPEG_LIBRARIES})
else()
    target_link_libraries(dng_sdk ZLIB::ZLIB)
endif()

# Native library
# 2026-08-26 Ceyx restructure Round 2: pipeline sources live under src/pipeline/,
# FFI sources under src/ffi/, generator sources moved entirely out of SRC_DIR to
# native/generators/. GLOB_RECURSE already recurses into subdirectories, and every
# EXCLUDE REGEX below is anchored with a ".*/" prefix (path-independent basename
# match), so the resulting NATIVE_SOURCES set is unchanged by the move — verified
# by target-dump diff in the acceptance step, not just by inspection.
file(GLOB_RECURSE NATIVE_SOURCES "${SRC_DIR}/*.cpp")
# W7-4 (TD-17): Exclude entire research/ subdirectory from production dylib.
# Complements the Generator filter below — any new WarpUtils.cpp or helper
# placed under src/research/ is automatically excluded without updating regex.
# (Round 2: research/ and the Generator*.cpp files no longer exist under SRC_DIR
# at all post-move, so these three filters are now no-ops kept for safety/history.)
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/research/.*")
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Dng[A-Z][A-Za-z0-9]*Generator\\.cpp$")
# P17 T9: the generic-RAW Halide generators are named Raw*Generator.cpp, which
# the Dng-prefixed filter above does not match. Without this they get swept
# into dng_decoder_native by the GLOB_RECURSE and fail to compile (Halide
# Generator API + Halide.h are generator-only).
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Raw[A-Z][A-Za-z0-9]*Generator\\.cpp$")
# 2026-08-25 naming refactor: RectilinearWarpGenerator.cpp (renamed from its
# Dng-prefixed name) does not match the Dng-prefixed filter above and gets
# swept into dng_decoder_native by the GLOB_RECURSE, causing undefined Halide
# Generator API symbols at dylib link time.
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Rectilinear[A-Z][A-Za-z0-9]*Generator\\.cpp$")
# P17 R5 (F1): the CPU demosaic oracle is test-only. The kernel tests compile
# it themselves; keeping it in the dylib exported the reference symbols from
# the production ABI.
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/raw_demosaic_reference\\.cpp$")
# P17 T6: src/libraw_frontend.cpp includes libraw/libraw.h, which is only on an
# include path when DNG_ENABLE_GENERIC_RAW is ON (libraw_vendored, below).
# The GLOB_RECURSE above would otherwise sweep it into dng_decoder_native in
# OFF builds too and break them.
if(NOT DNG_ENABLE_GENERIC_RAW)
    list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/libraw_(frontend|gpu_input_adapter)\\.cpp$")
    # P17 T10: the generic RAW route and its C ABI call into the frontend TUs
    # filtered out above, so they must drop out of OFF builds together.
    list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/raw_(gpu_pipeline|ffi_api)\\.cpp$")
endif()
# W6 H-3: bridge .cpp moved from dng_sdk_custom/source/ into src/;
# now auto-discovered by file(GLOB_RECURSE) above.
# Only add target if there are sources, dummy for now to avoid target creation error
if (NOT NATIVE_SOURCES)
    file(WRITE ${SRC_DIR}/dummy.cpp "void dummy(){}")
    set(NATIVE_SOURCES ${SRC_DIR}/dummy.cpp)
endif()

add_library(dng_decoder_native SHARED ${NATIVE_SOURCES})
target_include_directories(dng_decoder_native PUBLIC
    ${INC_DIR}
    ${SRC_DIR}/pipeline
    ${DNG_SDK_DIR})
if(DNG_USE_LIBJPEG)
    target_link_libraries(dng_decoder_native dng_sdk ${JPEG_LIBRARIES})
else()
    target_link_libraries(dng_decoder_native dng_sdk)
endif()

endif() # NOT DNG_HOST_GENERATORS_ONLY
