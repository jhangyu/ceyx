cmake_minimum_required(VERSION 3.14)
project(dng_decoder_native VERSION 1.0.0 LANGUAGES CXX C)

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

find_package(ZLIB REQUIRED)

# Use libjpeg-turbo for SIMD-accelerated JPEG decoding
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg PATHS /opt/homebrew/opt/jpeg-turbo/lib NO_DEFAULT_PATH)
if(TURBOJPEG_LIBRARY)
    message(STATUS "Found TurboJPEG: ${TURBOJPEG_LIBRARY}")
    set(TURBOJPEG_INCLUDE_DIRS "/opt/homebrew/opt/jpeg-turbo/include")
    add_compile_definitions(USE_LIBJPEG_TURBO=1)
else()
    find_package(JPEG REQUIRED)
    message(WARNING "TurboJPEG not found, lossless JPEG acceleration disabled")
endif()

# Adobe DNG SDK
set(DNG_SDK_DIR ${THIRD_PARTY_DIR}/dng_sdk/source)
file(GLOB_RECURSE DNG_SOURCES "${DNG_SDK_DIR}/*.cpp")
list(REMOVE_ITEM DNG_SOURCES "${DNG_SDK_DIR}/dng_xmp_sdk.cpp" "${DNG_SDK_DIR}/dng_xmp.cpp")

# DNG SDK custom implementations (Halide-accelerated)
# Note: dng_mosaic_halide.cpp is built into test targets that need Stage3 demosaic.
# Its default path uses the dng_demosaic_bilinear AOT kernel with CPU fallback.

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
target_include_directories(dng_sdk PUBLIC ${DNG_SDK_DIR} ${INC_DIR})
target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=1)
target_include_directories(dng_sdk PUBLIC ${JPEG_INCLUDE_DIRS})
if(TURBOJPEG_LIBRARY)
    target_include_directories(dng_sdk PUBLIC ${TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(dng_sdk ZLIB::ZLIB ${TURBOJPEG_LIBRARY})
else()
    target_link_libraries(dng_sdk ZLIB::ZLIB ${JPEG_LIBRARIES})
endif()

# Native library
file(GLOB_RECURSE NATIVE_SOURCES "${SRC_DIR}/*.cpp")
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
if(TURBOJPEG_LIBRARY)
    target_include_directories(dng_decoder_native PUBLIC ${TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(dng_decoder_native dng_sdk ${TURBOJPEG_LIBRARY})
else()
    target_link_libraries(dng_decoder_native dng_sdk ${JPEG_LIBRARIES})
endif()

set(HALIDE_DIR ${THIRD_PARTY_DIR}/halide)
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

set(HALIDE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/halide_generated)
file(MAKE_DIRECTORY ${HALIDE_OUTPUT_DIR})

if(APPLE)
    set(AOT_TARGET "host-metal-no_asserts-no_bounds_query")
else()
    set(AOT_TARGET "host-no_asserts-no_bounds_query")
endif()

set(DNG_RENDER_STAGE4_AOT_TARGET "host-metal-no_asserts-no_bounds_query-no_runtime")
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

target_include_directories(dng_decoder_native PUBLIC ${HALIDE_OUTPUT_DIR})
add_dependencies(dng_decoder_native dng_halide_aot_target)
add_dependencies(dng_decoder_native dng_demosaic_aot_target)
add_dependencies(dng_decoder_native dng_demosaic_warp_aot_target)
add_dependencies(dng_decoder_native dng_warp_aot_target)
if(DNG_ENABLE_WARP_PRECOMPUTED_DIAG)
    add_dependencies(dng_decoder_native dng_warp_precomputed_aot_target)
endif()
add_dependencies(dng_decoder_native dng_render_aot_target)
add_dependencies(dng_decoder_native dng_opcode_polynomial_aot_target)
add_dependencies(dng_decoder_native dng_opcode_polynomial3_aot_target)

# Halide setup — We only need Halide::Runtime for AOT, but Halide::Halide is fine
# CRITICAL: dng_pipeline.a MUST be linked (it is the only AOT without -no_runtime,
# providing Halide Metal runtime symbols: _halide_copy_to_device, _halide_malloc,
# _halide_metal_run, etc.). Removing it causes undefined symbol linker errors.
# All other AOT kernels use -no_runtime and depend on dng_pipeline.a for runtime.
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)
target_link_libraries(dng_decoder_native Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a)
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
if(TURBOJPEG_LIBRARY)
    target_include_directories(test_decode PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${TURBOJPEG_LIBRARY})
else()
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.a ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.a ${JPEG_LIBRARIES})
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
target_link_libraries(test_mosaic_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
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
target_link_libraries(test_mosaic_debug dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.a)
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
