cmake_minimum_required(VERSION 3.14)
project(dng_decoder_native VERSION 1.0.0 LANGUAGES CXX C)

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
# Android NDK sysroot does not ship libjpeg, so we make it optional on Android.
if(ANDROID)
    find_package(JPEG QUIET)
    if(NOT JPEG_FOUND)
        message(WARNING "libjpeg not found in NDK sysroot. "
            "Setting qDNGUseLibJPEG=0 — lossy DNG JPEG tile decode will be disabled. "
            "To enable: provide libjpeg via add_subdirectory or -DJPEG_LIBRARY/-DJPEG_INCLUDE_DIR.")
        set(DNG_USE_LIBJPEG OFF)
    else()
        set(DNG_USE_LIBJPEG ON)
    endif()
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

# Round 2 cleanup (Task #6): the precomputed-coords variant of rectilinear_warp
# was a Phase 8.1.6 diagnostic oracle (host-CPU double-precision baseline used
# to A/B against Metal fast-math drift). It is NOT part of the production decode
# path — production always uses the regular `rectilinear_warp` Metal kernel.
# When OFF (default) the precomputed AOT artifact is not generated, the dylib
# does not link it, and `dng_warp_halide.cpp` compiles out the include and call
# site so no `rectilinear_warp_precomputed` symbols are emitted. Flip ON only
# when running `test_warp_rectilinear_halide` for parity research.
option(DNG_ENABLE_WARP_PRECOMPUTED_DIAG
       "Build & link the rectilinear_warp_precomputed diagnostic oracle (off by default)"
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

# Phase 14: Force diagnostic options OFF on Android (no Metal backend)
if(ANDROID)
    set(DNG_ENABLE_WARP_PRECOMPUTED_DIAG OFF CACHE BOOL "" FORCE)
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

# DNG SDK custom implementations (Halide-accelerated)
    # Note: dng_mosaic_halide.cpp is built into targets that need Stage3 demosaic.
    # Production/test_decode call the AOT entry directly and fail fast if GPU is unsupported.

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
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
list(APPEND NATIVE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source/dng_mosaic_halide.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source/dng_warp_halide.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source/dng_render_halide.cpp")
# Only add target if there are sources, dummy for now to avoid target creation error
if (NOT NATIVE_SOURCES)
    file(WRITE ${SRC_DIR}/dummy.cpp "void dummy(){}")
    set(NATIVE_SOURCES ${SRC_DIR}/dummy.cpp)
endif()

add_library(dng_decoder_native SHARED ${NATIVE_SOURCES})
target_include_directories(dng_decoder_native PUBLIC
    ${INC_DIR}
    ${SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
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

    # AOT Generator for Halide Pipeline
    add_executable(dng_generator ${SRC_DIR}/DngHalideGenerator.cpp)
    target_include_directories(dng_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_generator PRIVATE Halide::Generator)

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
set(DNG_RENDER_STAGE4_AOT_TARGET "${AOT_TARGET}-no_runtime")
set(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE "-1" CACHE STRING
    "Android Stage4 diagnostic stage (-1 full render)")

# =============================================================================
# AOT custom commands (only when NOT cross-building — cross-build uses prebuilt)
# =============================================================================
if(NOT DNG_CROSS_BUILD)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_pipeline.h
    COMMAND dng_generator -g dng_pipeline -f dng_pipeline -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_generator
    COMMENT "Generating Halide AOT Pipeline..."
)
add_custom_target(dng_halide_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_pipeline.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.h
    COMMAND dng_warp_generator -g rectilinear_warp -f rectilinear_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_warp_generator
    COMMENT "Generating Halide AOT Rectilinear Warp..."
)
add_custom_target(dng_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a)

# Phase 8.1.6 D-1: precomputed-coords AOT variant. Same generator binary, but
# reads host-CPU double-precision base_x/base_y/frac_idx from input buffers
# instead of running the polynomial on GPU. Used to bypass Metal fast-math/FMA
# drift on R/B chromatic aberration polynomial.
# Round 2 (Task #6): gated behind DNG_ENABLE_WARP_PRECOMPUTED_DIAG (OFF by
# default). When OFF, the artifact is not generated and the dylib doesn't link
# it; dng_warp_halide.cpp also compiles out the call site (see the same macro
# guard there).
if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.h
        COMMAND dng_warp_generator -g rectilinear_warp -f rectilinear_warp_precomputed -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime precompute_coords=true
        DEPENDS dng_warp_generator
        COMMENT "Generating Halide AOT Rectilinear Warp (precomputed coords)..."
    )
    add_custom_target(dng_warp_precomputed_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.a)
endif()

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.h
    COMMAND dng_demosaic_generator -g dng_demosaic_bilinear -f dng_demosaic_bilinear -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_demosaic_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic..."
)
add_custom_target(dng_demosaic_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.h
    COMMAND dng_demosaic_warp_generator -g dng_demosaic_warp -f dng_demosaic_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime fast_codegen=$<IF:$<BOOL:${DNG_DEMOSAIC_WARP_FAST_CODEGEN}>,true,false>
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
        ${HALIDE_OUTPUT_DIR}/dng_pipeline.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
    )
    if(ANDROID OR (DNG_AOT_TARGET_OVERRIDE AND DNG_AOT_TARGET_OVERRIDE MATCHES "android"))
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a)
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
    add_dependencies(dng_decoder_native dng_halide_aot_target)
    add_dependencies(dng_decoder_native dng_demosaic_aot_target)
    add_dependencies(dng_decoder_native dng_demosaic_warp_aot_target)
    add_dependencies(dng_decoder_native dng_warp_aot_target)
    if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
        add_dependencies(dng_decoder_native dng_warp_precomputed_aot_target)
    endif()
    add_dependencies(dng_decoder_native dng_render_aot_target)
    if(TARGET dng_render_android_aot_target)
        add_dependencies(dng_decoder_native dng_render_android_aot_target)
    endif()
    add_dependencies(dng_decoder_native dng_opcode_polynomial_aot_target)
    add_dependencies(dng_decoder_native dng_opcode_polynomial3_aot_target)
endif()

# Halide setup — We only need Halide::Runtime for AOT, but Halide::Halide is fine
# CRITICAL: dng_pipeline.a MUST be linked (it is the only AOT without -no_runtime,
# providing Halide Metal/Vulkan runtime symbols: _halide_copy_to_device, _halide_malloc,
# _halide_metal_run / _halide_vulkan_run, etc.). Removing it causes undefined symbol
# linker errors. All other AOT kernels use -no_runtime and depend on dng_pipeline.a
# for runtime.
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)

# Phase 14: Halide::Halide (host libHalide) is only needed for native host builds.
# Android cross-build should NOT link host-arch libHalide.a.
if(NOT DNG_CROSS_BUILD AND NOT ANDROID)
    target_link_libraries(dng_decoder_native Halide::Halide)
endif()

# AOT artifacts (always linked — either freshly built or prebuilt)
target_link_libraries(dng_decoder_native
    ${HALIDE_OUTPUT_DIR}/dng_pipeline.a
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
if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
    target_compile_definitions(dng_decoder_native PRIVATE DNG_ENABLE_WARP_PRECOMPUTED_DIAG=1)
    target_link_libraries(dng_decoder_native ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.a)
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
        dng_sdk_custom/source/dng_mosaic_halide.cpp
        dng_sdk_custom/source/dng_warp_halide.cpp
        dng_sdk_custom/source/dng_render_halide.cpp)
    target_include_directories(test_decode_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_decode_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/dng_pipeline.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(test_decode_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(test_decode_android PRIVATE ${JPEG_LIBRARIES})
    endif()
    add_dependencies(test_decode_android test_android_vulkan_capability)
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


# W5-4 / TD-2: Device handoff path independent test.
# Validates that Stage3→Stage4 (lossless) and Stage2→Stage4 (lossy) device
# handoff paths produce bit-identical (PSNR ≥99dB) output vs host-copy fallback.
# NOTE: dng_pipeline.a provides Halide Metal runtime symbols (Gotcha #79, #64):
#   all other -no_runtime AOT kernels link against it for halide_metal_run etc.
add_executable(test_device_handoff tests/test_device_handoff.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    dng_sdk_custom/source/dng_mosaic_halide.cpp
    dng_sdk_custom/source/dng_warp_halide.cpp
    dng_sdk_custom/source/dng_render_halide.cpp)
target_include_directories(test_device_handoff PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
endif()
add_dependencies(test_device_handoff dng_halide_aot_target)
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
    dng_sdk_custom/source/dng_mosaic_halide.cpp
    dng_sdk_custom/source/dng_warp_halide.cpp
    dng_sdk_custom/source/dng_render_halide.cpp)
target_include_directories(test_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
endif()
if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
    target_compile_definitions(test_decode PRIVATE DNG_ENABLE_WARP_PRECOMPUTED_DIAG=1)
    target_link_libraries(test_decode ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.a)
    add_dependencies(test_decode dng_warp_precomputed_aot_target)
endif()
add_dependencies(test_decode dng_halide_aot_target)
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
add_executable(test_mosaic_halide tests/test_mosaic_halide.cpp
    dng_sdk_custom/source/dng_mosaic_halide.cpp)
target_include_directories(test_mosaic_halide PRIVATE
    ${INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W7-5 (TD-27): dng_pipeline.a is the Halide Metal runtime anchor (no -no_runtime).
# Without it, -no_runtime AOT kernels (rectilinear_warp, dng_demosaic_bilinear)
# fail to link with undefined Metal runtime symbols on Halide upgrades.
target_link_libraries(test_mosaic_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
add_dependencies(test_mosaic_halide dng_halide_aot_target)
add_dependencies(test_mosaic_halide dng_warp_aot_target)
add_dependencies(test_mosaic_halide dng_demosaic_aot_target)
if(APPLE)
    target_link_libraries(test_mosaic_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Debug demosaic test
add_executable(test_mosaic_debug tests/test_mosaic_debug.cpp
    dng_sdk_custom/source/dng_mosaic_halide.cpp)
target_include_directories(test_mosaic_debug PRIVATE
    ${INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W7-5 (TD-27): same runtime anchor as test_mosaic_halide above.
target_link_libraries(test_mosaic_debug dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
add_dependencies(test_mosaic_debug dng_halide_aot_target)
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

# Round 2 (Task #6): test_warp_rectilinear_halide is the diagnostic A/B
# oracle consumer for the precomputed warp variant. Built only when the diag
# option is enabled (configure with -DDNG_ENABLE_WARP_PRECOMPUTED_DIAG=ON).
# Default builds skip this test target entirely.
if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
    add_executable(test_warp_rectilinear_halide
        tests/test_warp_rectilinear_halide.cpp
        src/dng_opcodelist2_halide.cpp
        dng_sdk_custom/source/dng_mosaic_halide.cpp
        dng_sdk_custom/source/dng_warp_halide.cpp)
    target_compile_definitions(test_warp_rectilinear_halide PRIVATE DNG_ENABLE_WARP_PRECOMPUTED_DIAG=1)
    target_include_directories(test_warp_rectilinear_halide PRIVATE
        ${INC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    # NOTE: dng_pipeline.a is included specifically because it is the only AOT
    # artifact compiled WITHOUT `-no_runtime` and therefore provides the
    # Halide Metal runtime symbols (halide_metal_run, *_initialize_kernels,
    # *_finalize_kernels, *_device_interface, halide_copy_to_device) that the
    # other AOT .a files declare as undefined. Without it, linking this test
    # fails with "symbol(s) not found" on those runtime entry points.
    target_link_libraries(test_warp_rectilinear_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp_precomputed.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
    add_dependencies(test_warp_rectilinear_halide dng_halide_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_demosaic_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_demosaic_warp_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_warp_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_warp_precomputed_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_opcode_polynomial_aot_target)
    add_dependencies(test_warp_rectilinear_halide dng_opcode_polynomial3_aot_target)
    if(APPLE)
        target_link_libraries(test_warp_rectilinear_halide
            ${COREFOUNDATION_LIBRARY}
            ${CORESERVICES_LIBRARY}
            ${METAL_LIBRARY}
            ${FOUNDATION_LIBRARY})
    endif()
endif()

endif() # NOT DNG_CROSS_BUILD (test targets)

endif() # NOT DNG_HOST_GENERATORS_ONLY (entire runtime section)
