cmake_minimum_required(VERSION 3.14)
project(dng_decoder_native VERSION 1.0.0 LANGUAGES CXX C)

# Perf fix (2026-07-04): default to Release when the caller (script or IDE)
# does not set CMAKE_BUILD_TYPE. Without this, both the macOS host configure
# and the Android two-stage cross-compile in build_native_watchdog.py silently
# produced unoptimized, assert-enabled binaries (no -O flag, no -DNDEBUG).
# Explicit -DCMAKE_BUILD_TYPE=... from a caller (e.g. --cmake-arg) still wins.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default: Release)" FORCE)
endif()

# W7-2 (TD-4): Halide vendor version pin.
# See third_party/halide/VERSION for full provenance and ABI notes.
# Changing Halide version requires full AOT artifact regen (schedule ABI is not stable).
message(STATUS "Halide vendored: 21.0.0 (see third_party/halide/VERSION)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(APPLE)
    add_compile_definitions(qMacOS=1 qWinOS=0)
elseif(WIN32)
    add_compile_definitions(qMacOS=0 qWinOS=1)
elseif(ANDROID)
    add_compile_definitions(qAndroid=1 qMacOS=0 qWinOS=0)
elseif(UNIX)
    add_compile_definitions(qLinux=1 qMacOS=0 qWinOS=0)
endif()

# P14-W0B-07: Android NDK sysroot has libz.so but no CMake config file.
# Probe via find_library/find_path first; synthesize the imported target so
# the rest of the build can use ZLIB::ZLIB uniformly on all platforms.
if(ANDROID)
    find_library(ZLIB_LIBRARY_ANDROID z)
    find_path(ZLIB_INCLUDE_DIR_ANDROID zlib.h)
    if(ZLIB_LIBRARY_ANDROID AND ZLIB_INCLUDE_DIR_ANDROID)
        if(NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB SHARED IMPORTED)
            set_target_properties(ZLIB::ZLIB PROPERTIES
                IMPORTED_LOCATION "${ZLIB_LIBRARY_ANDROID}"
                INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR_ANDROID}")
        endif()
        set(ZLIB_FOUND TRUE)
        message(STATUS "Found zlib via NDK sysroot: ${ZLIB_LIBRARY_ANDROID}")
    endif()
endif()

if(NOT ZLIB_FOUND)
    find_package(ZLIB REQUIRED)
endif()

# JPEG library for DNG SDK lossy JPEG support (qDNGUseLibJPEG=1).
# Uses standard jpeglib.h API — any libjpeg-compatible library works.
# Android: build vendored libjpeg-turbo from source (NEON SIMD on arm64).
# macOS/host: STATICALLY link libjpeg-turbo (libjpeg.a).
#   Rationale (2026-08-17, P0 sandbox blocker): linking Homebrew's
#   libjpeg.dylib stamps an absolute LC_LOAD_DYLIB entry
#   (/opt/homebrew/opt/jpeg-turbo/lib/libjpeg.8.dylib) into
#   libdng_decoder_native.dylib. Any host app with App Sandbox enabled cannot
#   read /opt/homebrew, so dyld fails the dependency with
#   "file system sandbox blocked open()" and dlopen of our dylib fails outright.
#   Static linking removes the external dependency entirely (option B of
#   docs/logs/2026-08-17/handover-to-dng-decoder-team-libjpeg-blocker.md).
if(ANDROID)
    find_package(JPEG QUIET)
    if(NOT JPEG_FOUND)
        # Build libjpeg-turbo from vendored source for Android.
        # Minimal build: static library only, no turbojpeg API, no tools.
        set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
        set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
        set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
        set(WITH_SIMD ON CACHE BOOL "" FORCE)
        set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "libjpeg-turbo")
        add_subdirectory(${THIRD_PARTY_DIR}/libjpeg-turbo libjpeg-turbo-build EXCLUDE_FROM_ALL)
        # Expose headers (src/ has jpeglib.h etc, binary dir has generated jconfig.h)
        set(JPEG_INCLUDE_DIRS
            ${THIRD_PARTY_DIR}/libjpeg-turbo/src
            ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build)
        set(JPEG_LIBRARIES jpeg-static)
        message(STATUS "Building vendored libjpeg-turbo 3.1.0 for Android (NEON SIMD)")
    endif()
    set(DNG_USE_LIBJPEG ON)
elseif(APPLE)
    # Locate the static archive. Override with -DDNG_JPEG_TURBO_ROOT=<prefix>
    # if libjpeg-turbo lives outside the probed locations.
    set(DNG_JPEG_TURBO_ROOT "" CACHE PATH
        "libjpeg-turbo install prefix containing lib/libjpeg.a and include/jpeglib.h")
    if(NOT DNG_JPEG_TURBO_ROOT)
        find_program(DNG_BREW_EXECUTABLE brew)
        if(DNG_BREW_EXECUTABLE)
            execute_process(
                COMMAND ${DNG_BREW_EXECUTABLE} --prefix jpeg-turbo
                OUTPUT_VARIABLE DNG_BREW_JPEG_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE DNG_BREW_JPEG_RESULT)
            if(DNG_BREW_JPEG_RESULT EQUAL 0 AND DNG_BREW_JPEG_PREFIX)
                set(DNG_JPEG_TURBO_ROOT "${DNG_BREW_JPEG_PREFIX}" CACHE PATH "" FORCE)
            endif()
        endif()
    endif()

    find_library(DNG_JPEG_STATIC_LIBRARY
        NAMES libjpeg.a
        HINTS "${DNG_JPEG_TURBO_ROOT}/lib"
              /opt/homebrew/opt/jpeg-turbo/lib
              /usr/local/opt/jpeg-turbo/lib
        NO_DEFAULT_PATH)
    find_path(DNG_JPEG_STATIC_INCLUDE_DIR
        NAMES jpeglib.h
        HINTS "${DNG_JPEG_TURBO_ROOT}/include"
              /opt/homebrew/opt/jpeg-turbo/include
              /usr/local/opt/jpeg-turbo/include
        NO_DEFAULT_PATH)

    if(NOT DNG_JPEG_STATIC_LIBRARY OR NOT DNG_JPEG_STATIC_INCLUDE_DIR)
        message(FATAL_ERROR
            "Static libjpeg-turbo not found (need lib/libjpeg.a + include/jpeglib.h).\n"
            "  probed DNG_JPEG_TURBO_ROOT='${DNG_JPEG_TURBO_ROOT}'\n"
            "  libjpeg.a  = '${DNG_JPEG_STATIC_LIBRARY}'\n"
            "  jpeglib.h  = '${DNG_JPEG_STATIC_INCLUDE_DIR}'\n"
            "Install it (`brew install jpeg-turbo`) or pass "
            "-DDNG_JPEG_TURBO_ROOT=<prefix>.\n"
            "Do NOT fall back to the shared libjpeg.dylib: the resulting absolute "
            "dependency makes libdng_decoder_native.dylib unloadable inside "
            "App-Sandboxed host apps.")
    endif()

    set(JPEG_LIBRARIES ${DNG_JPEG_STATIC_LIBRARY})
    set(JPEG_INCLUDE_DIRS ${DNG_JPEG_STATIC_INCLUDE_DIR})
    set(DNG_USE_LIBJPEG ON)
    message(STATUS "Using STATIC libjpeg-turbo: ${DNG_JPEG_STATIC_LIBRARY}")
else()
    find_package(JPEG REQUIRED)
    set(DNG_USE_LIBJPEG ON)
endif()

# =============================================================================
# Options
# =============================================================================
option(DNG_DEMOSAIC_WARP_FAST_CODEGEN
       "Use fast 8x8 GPU schedule for fused demosaic+warp AOT; OFF uses 16x16 + channel unroll schedule"
       ON)
option(DNG_DEMOSAIC_WARP_STRICT_FLOAT
       "Append -strict_float to dng_demosaic_warp AOT target (Vulkan FP contraction probe); default OFF = identical to previous behaviour"
       OFF)


option(DNG_DIAGNOSTIC_BUILD
       "Build deprecated research generators (DngWarpStrictFloatGenerator, DngWarpDebugGenerator) for diagnostics"
       OFF)

# --- Phase 14: Cross-compile and AOT target options ---
option(DNG_HOST_GENERATORS_ONLY
       "Only build host-side Halide generators and run AOT (Stage 1 of cross-compile)"
       OFF)

option(DNG_CROSS_BUILD
       "Cross-compile mode: skip generator compilation, use prebuilt AOT artifacts"
       OFF)

set(DNG_PREBUILT_AOT_DIR "" CACHE PATH
    "Path to prebuilt AOT artifacts (used when DNG_CROSS_BUILD=ON)")

set(DNG_AOT_TARGET_OVERRIDE "" CACHE STRING
    "Override AOT target string (e.g. arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query)")

# R3-3: Halide Vulkan runtime fork with VkPipelineCache cross-launch
# persistence (Android only). OFF = stock runtime, feature cleanly absent.
# See native/halide_runtime_fork/README.md.
option(DNG_VK_PIPELINE_CACHE
       "R3-3: fork the Halide Vulkan runtime to persist VkPipelineCache across launches (Android only; OFF = stock runtime)"
       ON)

# Phase 14: Force diagnostic options OFF on Android (no Metal backend)
if(ANDROID)
    set(DNG_DIAGNOSTIC_BUILD OFF CACHE BOOL "" FORCE)
endif()

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
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

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
target_compile_options(dng_sdk PRIVATE -ffp-contract=off)
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
file(GLOB_RECURSE NATIVE_SOURCES "${SRC_DIR}/*.cpp")
# W7-4 (TD-17): Exclude entire research/ subdirectory from production dylib.
# Complements the Generator filter below — any new WarpUtils.cpp or helper
# placed under src/research/ is automatically excluded without updating regex.
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/research/.*")
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Dng[A-Z][A-Za-z0-9]*Generator\\.cpp$")
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
    ${SRC_DIR}
    ${DNG_SDK_DIR})
if(DNG_USE_LIBJPEG)
    target_link_libraries(dng_decoder_native dng_sdk ${JPEG_LIBRARIES})
else()
    target_link_libraries(dng_decoder_native dng_sdk)
endif()

endif() # NOT DNG_HOST_GENERATORS_ONLY

# =============================================================================
# Halide: generators, AOT commands, and linking
# =============================================================================
set(HALIDE_DIR ${THIRD_PARTY_DIR}/halide)

# Phase 14: In cross-build mode, skip Halide find_package and all generator targets.
# We only need Halide headers for AOT-generated .h includes.
if(NOT DNG_CROSS_BUILD)
    list(APPEND CMAKE_PREFIX_PATH ${HALIDE_DIR})
    find_package(Halide REQUIRED)

    # W6 M-4: DngHalideGenerator.cpp (legacy dng_pipeline all-in-one kernel) archived.
    # The standalone halide_runtime.a below replaces its runtime-anchor role.
    # Source retained in src/ for reference; not compiled.

    add_executable(dng_warp_generator ${SRC_DIR}/DngWarpGenerator.cpp)
    target_include_directories(dng_warp_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_warp_generator PRIVATE Halide::Generator)

    add_executable(dng_demosaic_generator ${SRC_DIR}/DngDemosaicGenerator.cpp)
    target_include_directories(dng_demosaic_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_demosaic_generator PRIVATE Halide::Generator)

    add_executable(dng_demosaic_warp_generator ${SRC_DIR}/DngDemosaicWarpGenerator.cpp)
    target_include_directories(dng_demosaic_warp_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_demosaic_warp_generator PRIVATE Halide::Generator)

    # Research & diagnostic generators (only built if DNG_DIAGNOSTIC_BUILD=ON)
    if(DNG_DIAGNOSTIC_BUILD)
        add_executable(dng_warp_strict_float_generator ${SRC_DIR}/research/DngWarpStrictFloatGenerator.cpp)
        target_include_directories(dng_warp_strict_float_generator PRIVATE ${INC_DIR})
        target_link_libraries(dng_warp_strict_float_generator PRIVATE Halide::Generator)

        add_executable(dng_warp_debug_generator ${SRC_DIR}/research/DngWarpDebugGenerator.cpp)
        target_include_directories(dng_warp_debug_generator PRIVATE ${INC_DIR})
        target_link_libraries(dng_warp_debug_generator PRIVATE Halide::Generator)
    endif()

    add_executable(dng_render_generator ${SRC_DIR}/DngRenderGenerator.cpp)
    target_include_directories(dng_render_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_render_generator PRIVATE Halide::Generator)

    # Phase 10 Sprint C1: MapPolynomial AOT generator (Stage 2 OpcodeList2 GPU).
    add_executable(dng_opcode_polynomial_generator
        ${SRC_DIR}/DngOpcodePolynomialGenerator.cpp)
    target_link_libraries(dng_opcode_polynomial_generator PRIVATE Halide::Generator)

    add_executable(dng_opcode_polynomial3_generator
        ${SRC_DIR}/DngOpcodePolynomial3Generator.cpp)
    target_link_libraries(dng_opcode_polynomial3_generator PRIVATE Halide::Generator)

endif() # NOT DNG_CROSS_BUILD

# =============================================================================
# Phase 14: AOT output directory and target string
# =============================================================================
if(DNG_CROSS_BUILD)
    if(NOT DNG_PREBUILT_AOT_DIR)
        message(FATAL_ERROR "DNG_CROSS_BUILD=ON requires DNG_PREBUILT_AOT_DIR to be set")
    endif()
    set(HALIDE_OUTPUT_DIR "${DNG_PREBUILT_AOT_DIR}")
else()
    set(HALIDE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/halide_generated)
    file(MAKE_DIRECTORY ${HALIDE_OUTPUT_DIR})
endif()

# --- Phase 14: Platform-specific Halide AOT target strings ---
if(DNG_AOT_TARGET_OVERRIDE)
    set(AOT_TARGET "${DNG_AOT_TARGET_OVERRIDE}")
else()
    if(APPLE)
        set(AOT_TARGET "host-metal-no_asserts-no_bounds_query")
    elseif(ANDROID)
        # Strict Android Vulkan device contract: arm64-v8a Vulkan with Halide vk_int8/vk_int16/vk_int64.
        set(AOT_TARGET "arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query")
    else()
        set(AOT_TARGET "host-no_asserts-no_bounds_query")
    endif()
endif()

# Stage4 render uses same base target with -no_runtime (fixes hardcoded Metal bug)
# Route A adoption (P16, 2026-06-12): the macOS Metal production Stage4 render is
# pinned to IEEE stepwise via strict_float so its output is byte-exact against the
# -ffp-contract=off SDK render reference (dng_reference.cpp, see below). The Android
# Vulkan target is intentionally left unchanged — Halide v21 Vulkan
# unstrictify_float() drops strict semantics, making strict_float a no-op there
# (documented semantic gap; Android re-verified in W7 integration). Generic host
# CPU build also unchanged.
if(APPLE AND NOT ANDROID AND NOT (DNG_AOT_TARGET_OVERRIDE AND DNG_AOT_TARGET_OVERRIDE MATCHES "android"))
    set(DNG_RENDER_STAGE4_AOT_TARGET "${AOT_TARGET}-strict_float-no_runtime")
else()
    set(DNG_RENDER_STAGE4_AOT_TARGET "${AOT_TARGET}-no_runtime")
endif()
set(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE "-1" CACHE STRING
    "Android Stage4 diagnostic stage (-1 full render)")

# =============================================================================
# AOT custom commands (only when NOT cross-building — cross-build uses prebuilt)
# =============================================================================
if(NOT DNG_CROSS_BUILD)

# W6 M-4: Standalone Halide runtime replaces legacy dng_pipeline.a runtime anchor.
# Any generator binary can emit the runtime via `-r`; we use dng_demosaic_generator.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/halide_runtime.a
    COMMAND dng_demosaic_generator -r halide_runtime -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_demosaic_generator
    COMMENT "Generating standalone Halide runtime..."
)
add_custom_target(halide_runtime_target DEPENDS ${HALIDE_OUTPUT_DIR}/halide_runtime.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.h
    COMMAND dng_warp_generator -g rectilinear_warp -f rectilinear_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_warp_generator
    COMMENT "Generating Halide AOT Rectilinear Warp..."
)
add_custom_target(dng_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.h
    COMMAND dng_demosaic_generator -g dng_demosaic_bilinear -f dng_demosaic_bilinear -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_demosaic_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic..."
)
add_custom_target(dng_demosaic_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.h
    COMMAND dng_demosaic_warp_generator -g dng_demosaic_warp -f dng_demosaic_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime$<$<BOOL:${DNG_DEMOSAIC_WARP_STRICT_FLOAT}>:-strict_float> fast_codegen=$<IF:$<BOOL:${DNG_DEMOSAIC_WARP_FAST_CODEGEN}>,true,false>
    DEPENDS dng_demosaic_warp_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic+Warp..."
)
add_custom_target(dng_demosaic_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.h
    COMMAND dng_render_generator -g dng_render_stage4 -f dng_render_stage4 -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render..."
)
add_custom_target(dng_render_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a)

# Phase 14: Android Vulkan three-channel-split Stage4 generator.
# Eliminates Tuple + dim(2) codegen that triggers Halide v21 SPIR-V R==G bug.
if(ANDROID OR (DNG_AOT_TARGET_OVERRIDE AND DNG_AOT_TARGET_OVERRIDE MATCHES "android"))
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.h
        COMMAND dng_render_generator -g dng_render_stage4_android -f dng_render_stage4_android -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET} diag_stage=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render (Android 3-channel split)..."
    )
    add_custom_target(dng_render_android_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a)

    # P14-W4-4 GO/NO-GO probe: isolated interleaved flat-1D src-read AOT.
    # Separate kernel/signature; does not touch the production Stage4 kernel.
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.h
        COMMAND dng_render_generator -g dng_render_stage4_android_probe -f dng_render_stage4_android_probe -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render PROBE (Android interleaved-src go/no-go)..."
    )
    add_custom_target(dng_render_android_probe_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.a)
endif()

# Phase 10 Sprint C1: MapPolynomial AOT (Stage 2 OpcodeList2 GPU).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.h
    COMMAND dng_opcode_polynomial_generator -g dng_opcode_polynomial
        -f dng_opcode_polynomial -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.h
    COMMAND dng_opcode_polynomial3_generator -g dng_opcode_polynomial3
        -f dng_opcode_polynomial3 -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial3_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 batched MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial3_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)

# When building host generators only (Stage 1), pull AOT targets into ALL
# so `cmake --build` actually runs the generators.
if(DNG_HOST_GENERATORS_ONLY)
    set(_DNG_ALL_AOT_DEPS
        ${HALIDE_OUTPUT_DIR}/halide_runtime.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
    )
    if(ANDROID OR (DNG_AOT_TARGET_OVERRIDE AND DNG_AOT_TARGET_OVERRIDE MATCHES "android"))
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a)
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.a)
    endif()
    add_custom_target(dng_all_aot ALL DEPENDS ${_DNG_ALL_AOT_DEPS})
endif()

endif() # NOT DNG_CROSS_BUILD (AOT custom commands)

# =============================================================================
# Link AOT artifacts and platform libraries into dng_decoder_native
# =============================================================================
if(NOT DNG_HOST_GENERATORS_ONLY)

target_include_directories(dng_decoder_native PUBLIC ${HALIDE_OUTPUT_DIR})

if(NOT DNG_CROSS_BUILD)
    add_dependencies(dng_decoder_native halide_runtime_target)
    add_dependencies(dng_decoder_native dng_demosaic_aot_target)
    add_dependencies(dng_decoder_native dng_demosaic_warp_aot_target)
    add_dependencies(dng_decoder_native dng_warp_aot_target)
    add_dependencies(dng_decoder_native dng_render_aot_target)
    if(TARGET dng_render_android_aot_target)
        add_dependencies(dng_decoder_native dng_render_android_aot_target)
    endif()
    add_dependencies(dng_decoder_native dng_opcode_polynomial_aot_target)
    add_dependencies(dng_decoder_native dng_opcode_polynomial3_aot_target)
endif()

# Halide setup — We only need Halide::Runtime for AOT, but Halide::Halide is fine
# W6 M-4: halide_runtime.a is the standalone Halide runtime (Metal/Vulkan backend
# symbols: _halide_copy_to_device, _halide_malloc, _halide_metal_run, etc.).
# All AOT kernels use -no_runtime and link against halide_runtime.a.
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)

# W6 M-4 completion: Halide::Halide (host libHalide, 196MB shared lib) is no longer
# linked into dng_decoder_native. The standalone halide_runtime.a provides all AOT
# runtime symbols statically. Halide::Halide remains linked by generator executables
# and test targets that use the Halide host API directly.

# AOT artifacts (always linked — either freshly built or prebuilt)
target_link_libraries(dng_decoder_native
    ${HALIDE_OUTPUT_DIR}/halide_runtime.a
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
    ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
if(ANDROID)
    target_link_libraries(dng_decoder_native
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a)
    target_compile_definitions(dng_decoder_native PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
endif()
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(CORESERVICES_LIBRARY CoreServices)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    target_link_libraries(dng_decoder_native
        ${COREFOUNDATION_LIBRARY}
        ${CORESERVICES_LIBRARY}
        ${METAL_LIBRARY}        # Required for Halide Metal GPU backend
        ${FOUNDATION_LIBRARY}   # Required for Halide Metal GPU backend
    )

    set_target_properties(dng_decoder_native PROPERTIES
        FRAMEWORK FALSE
        MACOSX_RPATH TRUE
    )
elseif(ANDROID)
    find_library(VULKAN_LIBRARY vulkan)
    if(NOT VULKAN_LIBRARY)
        message(FATAL_ERROR "Vulkan not found in NDK sysroot. Ensure ANDROID_PLATFORM >= android-24.")
    endif()
    find_library(LOG_LIBRARY log)
    target_link_libraries(dng_decoder_native ${VULKAN_LIBRARY} ${LOG_LIBRARY})
endif()

# Phase 14 W0 acceptance smoke: Android cross-build test binary for ADB.
# Full host test targets stay under the NOT DNG_CROSS_BUILD block below.
if(ANDROID AND DNG_CROSS_BUILD)
    add_executable(test_android_vulkan_capability
        tests/android_vulkan_capability_probe.cpp)
    target_link_libraries(test_android_vulkan_capability PRIVATE
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})

    add_executable(test_decode_android tests/test_decode.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(test_decode_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_decode_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(test_decode_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_STAGE4_INTERLEAVED_SRC_PROBE)
        target_compile_definitions(test_decode_android PRIVATE DNG_STAGE4_INTERLEAVED_SRC_PROBE=1)
    endif()
    if(DNG_USE_LIBJPEG)
        target_link_libraries(test_decode_android PRIVATE ${JPEG_LIBRARIES})
    endif()
    add_dependencies(test_decode_android test_android_vulkan_capability)

    # P14-W4-4 measurement: Android cross-build of the production C ABI harness.
    # Drives the FFI entry dng_decode_and_process -> dng_pipeline_v2_decode_to_rgb
    # -> decodeStages -> runHalideStage3And4Fused (the device-handoff path that
    # test_decode_android bypasses). Lets us measure on cc5bf709 whether device
    # handoff actually triggers and what [Stage4-Perf] FromDevice: reports.
    # Measurement-only target; no kernel / device-ownership code is touched.
    add_executable(dng_ffi_harness_android tests/dng_ffi_harness.cpp
        src/dng_ffi_api.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(dng_ffi_harness_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(dng_ffi_harness_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(dng_ffi_harness_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(dng_ffi_harness_android PRIVATE ${JPEG_LIBRARIES})
    endif()

    # matrix-eng ask (2026-07-04, Task #3): Android cross-build of the device-handoff
    # PSNR gate (Stage3->Stage4 device-dirty handoff vs host-copy fallback), mirroring
    # dng_ffi_harness_android above but for tests/test_device_handoff.cpp. Uses the
    # Android/Vulkan Stage4 AOT variant (dng_render_stage4_android.a), not the Metal
    # one linked by the macOS-only test_device_handoff target below.
    add_executable(test_device_handoff_android tests/test_device_handoff.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(test_device_handoff_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_device_handoff_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(test_device_handoff_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(test_device_handoff_android PRIVATE ${JPEG_LIBRARIES})
    endif()

    # ------------------------------------------------------------------------
    # R3-3: Halide Vulkan runtime fork (VkPipelineCache persistence).
    # See native/halide_runtime_fork/README.md for the weak-override mechanism.
    # The fork object is post-processed with llvm-objcopy --weaken: the fork TU
    # and halide_runtime.a both define ~271 internal helper methods STRONG
    # (LinkedList/BlockStorage/... — non-inline methods from src/runtime
    # internal headers); weakening the fork object lets the archive win those
    # (ABI-identical v21 sources) while the weak-vs-weak vulkan entry points
    # resolve to the fork (objects precede archives on the link line —
    # validated by scripts/tmp/r3_3_link_probe.sh disassembly evidence).
    # ------------------------------------------------------------------------
    if(DNG_VK_PIPELINE_CACHE)
        set(DNG_VK_FORK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/halide_runtime_fork)
        add_library(halide_vulkan_fork OBJECT ${DNG_VK_FORK_DIR}/vulkan.cpp)
        target_include_directories(halide_vulkan_fork PRIVATE
            ${DNG_VK_FORK_DIR}
            ${DNG_VK_FORK_DIR}/upstream)
        # Match the upstream Halide runtime build environment:
        # runtime_internal.h rejects hosted compiles; COMPILING_HALIDE_RUNTIME
        # makes HalideRuntime.h use runtime_internal.h typedefs.
        target_compile_definitions(halide_vulkan_fork PRIVATE COMPILING_HALIDE_RUNTIME)
        target_compile_options(halide_vulkan_fork PRIVATE
            -ffreestanding -fno-exceptions -fno-rtti)

        set(DNG_VK_FORK_WEAK_OBJ ${CMAKE_CURRENT_BINARY_DIR}/halide_vulkan_fork_weak.o)
        add_custom_command(
            OUTPUT ${DNG_VK_FORK_WEAK_OBJ}
            COMMAND ${CMAKE_OBJCOPY} --weaken $<TARGET_OBJECTS:halide_vulkan_fork> ${DNG_VK_FORK_WEAK_OBJ}
            DEPENDS halide_vulkan_fork $<TARGET_OBJECTS:halide_vulkan_fork>
            COMMENT "R3-3: weakening Halide Vulkan fork object (avoid dup-strong collisions with halide_runtime.a)"
            VERBATIM)
        add_custom_target(halide_vulkan_fork_weak DEPENDS ${DNG_VK_FORK_WEAK_OBJ})

        # dng_decoder_native (production .so) + the three Android test
        # binaries link halide_runtime.a directly, so all four need the fork
        # object — otherwise the FFI harness would benchmark the stock runtime.
        foreach(_dng_vkpc_tgt
                dng_decoder_native
                dng_ffi_harness_android
                test_decode_android
                test_device_handoff_android)
            if(TARGET ${_dng_vkpc_tgt})
                target_sources(${_dng_vkpc_tgt} PRIVATE ${DNG_VK_FORK_WEAK_OBJ})
                add_dependencies(${_dng_vkpc_tgt} halide_vulkan_fork_weak)
            endif()
        endforeach()
    endif()
endif()

# =============================================================================
# Test targets — only built for native host builds (not cross-compile, not
# generator-only). Phase 14: guarded to prevent Android/cross-compile breakage.
# =============================================================================
if(NOT DNG_CROSS_BUILD)

# P12-W0B-03: production C ABI verification harness.
add_executable(dng_ffi_harness tests/dng_ffi_harness.cpp)
target_include_directories(dng_ffi_harness PRIVATE ${INC_DIR})
target_link_libraries(dng_ffi_harness PRIVATE dng_decoder_native)

# 2026-08-16 CFA phase: end-to-end pixel color-correctness gate. Decodes a
# real DNG through the production C ABI and asserts a known channel relation
# (blue sky must have B >> R), which is the only observable a wrong Bayer
# phase corrupts. Driven by run_decode_matrix.py against an external sample.
add_executable(test_cfa_color tests/test_cfa_color.cpp)
target_include_directories(test_cfa_color PRIVATE ${INC_DIR})
target_link_libraries(test_cfa_color PRIVATE dng_decoder_native)


# W5-4 / TD-2: Device handoff path independent test.
# Validates that Stage3→Stage4 (lossless) and Stage2→Stage4 (lossy) device
# handoff paths produce bit-identical (PSNR ≥99dB) output vs host-copy fallback.
add_executable(test_device_handoff tests/test_device_handoff.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp
    src/dng_render_halide.cpp)
target_include_directories(test_device_handoff PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
endif()
add_dependencies(test_device_handoff halide_runtime_target)
add_dependencies(test_device_handoff dng_demosaic_aot_target)
add_dependencies(test_device_handoff dng_demosaic_warp_aot_target)
add_dependencies(test_device_handoff dng_warp_aot_target)
add_dependencies(test_device_handoff dng_render_aot_target)
add_dependencies(test_device_handoff dng_opcode_polynomial_aot_target)
add_dependencies(test_device_handoff dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_device_handoff ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()


# Color accuracy / visual regression test
add_executable(test_color_accuracy tests/test_color_accuracy.cpp)
target_include_directories(test_color_accuracy PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_color_accuracy dng_sdk)
if(APPLE)
    target_link_libraries(test_color_accuracy ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

add_executable(test_dng_layout tests/test_dng_layout.cpp)
target_include_directories(test_dng_layout PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_layout dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_layout ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# Tile testing tool
add_executable(test_dng_tiles tests/test_dng_tiles.cpp)
target_include_directories(test_dng_tiles PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_tiles dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_tiles ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()


add_executable(test_dng_preview tests/test_dng_preview.cpp)
target_include_directories(test_dng_preview PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_preview dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_preview ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# DNG SDK Decode Pipeline Test Tool (with Halide Stage3 demosaic)
add_executable(test_decode tests/test_decode.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp
    src/dng_render_halide.cpp)
target_include_directories(test_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
endif()
add_dependencies(test_decode halide_runtime_target)
add_dependencies(test_decode dng_demosaic_aot_target)
add_dependencies(test_decode dng_demosaic_warp_aot_target)
add_dependencies(test_decode dng_warp_aot_target)
add_dependencies(test_decode dng_render_aot_target)
add_dependencies(test_decode dng_opcode_polynomial_aot_target)
add_dependencies(test_decode dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Phase 5.3: Halide Demosaic PSNR Test
# 2026-08-16: dng_opcodelist2_halide.cpp / dng_halide_device.cpp added to the
# source list. libdng_sdk's dng_opcode_list::Apply references the Stage2
# OpcodeList2 Halide bridge symbols unconditionally, so this target had been
# failing to link since that bridge landed (no test_mosaic_halide binary was
# ever produced). Linking the bridge in restores the target; the polynomial
# AOT archives below satisfy the bridge's own kernel references.
add_executable(test_mosaic_halide tests/test_mosaic_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_halide_device.cpp)
target_include_directories(test_mosaic_halide PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W6 M-4: halide_runtime.a provides Metal/Vulkan runtime symbols for all -no_runtime AOT kernels.
target_link_libraries(test_mosaic_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
add_dependencies(test_mosaic_halide halide_runtime_target)
add_dependencies(test_mosaic_halide dng_warp_aot_target)
add_dependencies(test_mosaic_halide dng_demosaic_aot_target)
add_dependencies(test_mosaic_halide dng_opcode_polynomial_aot_target)
add_dependencies(test_mosaic_halide dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_mosaic_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# 2026-08-16 CFA phase: all-four-Bayer-phases unit check on a synthetic
# mosaic. Covers both the Halide AOT kernel and the CPU reference demosaic
# plus get_cfa_pattern's phase expansion. No DNG fixture required.
add_executable(test_cfa_phase tests/test_cfa_phase.cpp
    src/dng_mosaic_halide.cpp)
target_include_directories(test_cfa_phase PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# dng_sdk: the resolver case feeds dng_resolve_cfa_phase a synthetic dng_mosaic_info.
target_link_libraries(test_cfa_phase dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
add_dependencies(test_cfa_phase halide_runtime_target)
add_dependencies(test_cfa_phase dng_demosaic_aot_target)
if(APPLE)
    target_link_libraries(test_cfa_phase ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Debug demosaic test
add_executable(test_mosaic_debug tests/test_mosaic_debug.cpp
    src/dng_mosaic_halide.cpp)
target_include_directories(test_mosaic_debug PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W6 M-4: same standalone runtime as test_mosaic_halide above.
target_link_libraries(test_mosaic_debug dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
add_dependencies(test_mosaic_debug halide_runtime_target)
add_dependencies(test_mosaic_debug dng_warp_aot_target)
add_dependencies(test_mosaic_debug dng_demosaic_aot_target)
if(APPLE)
    target_link_libraries(test_mosaic_debug ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Phase 5.1: Halide Render PSNR Test (placeholder - needs DNG SDK data)
add_executable(test_render_halide tests/test_render_halide.cpp)
target_include_directories(test_render_halide PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR})
target_link_libraries(test_render_halide dng_sdk)
if(APPLE)
    target_link_libraries(test_render_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

endif() # NOT DNG_CROSS_BUILD (test targets)

endif() # NOT DNG_HOST_GENERATORS_ONLY (entire runtime section)
