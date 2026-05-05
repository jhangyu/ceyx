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
# Note: dng_mosaic_halide.cpp is built separately as it requires Halide JIT compilation
# It should be compiled as part of the test_decode or a dedicated target

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
target_include_directories(dng_sdk PUBLIC ${DNG_SDK_DIR})
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
# Only add target if there are sources, dummy for now to avoid target creation error
if (NOT NATIVE_SOURCES)
    file(WRITE ${SRC_DIR}/dummy.cpp "void dummy(){}")
    set(NATIVE_SOURCES ${SRC_DIR}/dummy.cpp)
endif()

add_library(dng_decoder_native SHARED ${NATIVE_SOURCES})
target_include_directories(dng_decoder_native PUBLIC ${INC_DIR})
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

add_executable(dng_render_generator ${SRC_DIR}/DngRenderGenerator.cpp)
target_include_directories(dng_render_generator PRIVATE ${INC_DIR})
target_link_libraries(dng_render_generator PRIVATE Halide::Generator)

add_executable(dng_render_nomap_generator ${SRC_DIR}/DngRenderNoMapGenerator.cpp)
target_include_directories(dng_render_nomap_generator PRIVATE ${INC_DIR})
target_link_libraries(dng_render_nomap_generator PRIVATE Halide::Generator)

add_executable(dng_render_maps_noencode_generator ${SRC_DIR}/DngRenderMapsNoEncodingGenerator.cpp)
target_include_directories(dng_render_maps_noencode_generator PRIVATE ${INC_DIR})
target_link_libraries(dng_render_maps_noencode_generator PRIVATE Halide::Generator)

add_executable(dng_render_tail_generator ${SRC_DIR}/DngRenderTailGenerator.cpp)
target_include_directories(dng_render_tail_generator PRIVATE ${INC_DIR})
target_link_libraries(dng_render_tail_generator PRIVATE Halide::Generator)
option(DNG_RENDER_TAIL_STRICT_R
       "Apply strict_float to tail rgb_to_final R-channel sum only"
       OFF)
option(DNG_RENDER_TAIL_STRICT_G
       "Apply strict_float to tail rgb_to_final G-channel sum only"
       OFF)
option(DNG_RENDER_TAIL_STRICT_B
       "Apply strict_float to tail rgb_to_final B-channel sum only"
       OFF)
if(DNG_RENDER_TAIL_STRICT_R)
    target_compile_definitions(dng_render_tail_generator PRIVATE DNG_RENDER_TAIL_STRICT_R=1)
endif()
if(DNG_RENDER_TAIL_STRICT_G)
    target_compile_definitions(dng_render_tail_generator PRIVATE DNG_RENDER_TAIL_STRICT_G=1)
endif()
if(DNG_RENDER_TAIL_STRICT_B)
    target_compile_definitions(dng_render_tail_generator PRIVATE DNG_RENDER_TAIL_STRICT_B=1)
endif()

add_executable(dng_render_tonetail_generator ${SRC_DIR}/DngRenderToneTailGenerator.cpp)
target_include_directories(dng_render_tonetail_generator PRIVATE ${INC_DIR})
target_link_libraries(dng_render_tonetail_generator PRIVATE Halide::Generator)

set(HALIDE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/halide_generated)
file(MAKE_DIRECTORY ${HALIDE_OUTPUT_DIR})

if(APPLE)
    set(AOT_TARGET "host-metal-no_asserts-no_bounds_query")
else()
    set(AOT_TARGET "host-no_asserts-no_bounds_query")
endif()

option(DNG_RENDER_STAGE4_STRICT_FLOAT
       "Enable strict_float target feature for dng_render_stage4 AOT only"
       OFF)
option(DNG_RENDER_TAIL_STRICT_FLOAT
       "Enable strict_float target feature for dng_render_tail_stage4 AOT only"
       OFF)
option(DNG_RENDER_STAGE4_USE_METAL_TARGET
       "Use metal feature in dng_render_stage4 AOT target (APPLE only)"
       ON)
option(DNG_RENDER_STAGE4_MATRIX_STRICT_EXPR
       "Enable strict_float intrinsic on rgb_to_final matrix expression in DngRenderGenerator"
       OFF)
option(DNG_RENDER_STAGE4_ENCODE_TRUNCATE
       "Use truncation (no +0.5) in Stage4 encode8 quantization"
       OFF)
option(DNG_RENDER_STAGE4_MATRIX_ASSOC_ALT
       "Use alternative association order for rgb_to_final accumulation"
       OFF)
option(DNG_RENDER_STAGE4_MATRIX_STRICT_R
       "Apply strict_float to rgb_to_final R-channel sum only"
       OFF)
option(DNG_RENDER_STAGE4_MATRIX_STRICT_G
       "Apply strict_float to rgb_to_final G-channel sum only"
       OFF)
option(DNG_RENDER_STAGE4_MATRIX_STRICT_B
       "Apply strict_float to rgb_to_final B-channel sum only"
       OFF)
option(DNG_RENDER_STAGE4_ABC_ASSOC_ALT
       "Use alternative association order for camera_to_rgb accumulation"
       OFF)
option(DNG_RENDER_STAGE4_FRONTEND_SPLIT_ABC
       "Materialize ABC output as an explicit stage (fusion boundary experiment)"
       OFF)
option(DNG_RENDER_STAGE4_FRONTEND_SPLIT_TONE
       "Materialize tone output as an explicit stage (fusion boundary experiment)"
       OFF)
option(DNG_RENDER_STAGE4_ABC_SPLIT_MULADD
       "Force camera_to_rgb matrix into split mul/add sequence"
       OFF)
option(DNG_RENDER_STAGE4_MATRIX_SPLIT_MULADD
       "Force rgb_to_final matrix into split mul/add sequence"
       OFF)
option(DNG_RENDER_STAGE4_NO_VECTORIZE
       "Disable Halide vectorize/unroll on CPU schedule for Stage4 (FMA/SIMD-reorder bisection)"
       OFF)
if(DNG_RENDER_STAGE4_MATRIX_STRICT_EXPR)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_STRICT_EXPR=1)
endif()
if(DNG_RENDER_STAGE4_ENCODE_TRUNCATE)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_ENCODE_TRUNCATE=1)
endif()
if(DNG_RENDER_STAGE4_MATRIX_ASSOC_ALT)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_ASSOC_ALT=1)
endif()
if(DNG_RENDER_STAGE4_MATRIX_STRICT_R)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_STRICT_R=1)
endif()
if(DNG_RENDER_STAGE4_MATRIX_STRICT_G)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_STRICT_G=1)
endif()
if(DNG_RENDER_STAGE4_MATRIX_STRICT_B)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_STRICT_B=1)
endif()
if(DNG_RENDER_STAGE4_ABC_ASSOC_ALT)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_ABC_ASSOC_ALT=1)
endif()
if(DNG_RENDER_STAGE4_FRONTEND_SPLIT_ABC)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_FRONTEND_SPLIT_ABC=1)
endif()
if(DNG_RENDER_STAGE4_FRONTEND_SPLIT_TONE)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_FRONTEND_SPLIT_TONE=1)
endif()
if(DNG_RENDER_STAGE4_ABC_SPLIT_MULADD)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_ABC_SPLIT_MULADD=1)
endif()
if(DNG_RENDER_STAGE4_MATRIX_SPLIT_MULADD)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_MATRIX_SPLIT_MULADD=1)
endif()
if(DNG_RENDER_STAGE4_NO_VECTORIZE)
    target_compile_definitions(dng_render_generator PRIVATE DNG_RENDER_STAGE4_NO_VECTORIZE=1)
endif()
if(APPLE AND DNG_RENDER_STAGE4_USE_METAL_TARGET)
    set(DNG_RENDER_STAGE4_AOT_BASE_TARGET "host-metal-no_asserts-no_bounds_query")
else()
    set(DNG_RENDER_STAGE4_AOT_BASE_TARGET "host-no_asserts-no_bounds_query")
endif()
set(DNG_RENDER_STAGE4_AOT_TARGET "${DNG_RENDER_STAGE4_AOT_BASE_TARGET}-no_runtime")
if(DNG_RENDER_STAGE4_STRICT_FLOAT)
    set(DNG_RENDER_STAGE4_AOT_TARGET "${DNG_RENDER_STAGE4_AOT_BASE_TARGET}-strict_float-no_runtime")
endif()
message(STATUS "dng_render_stage4 AOT target: ${DNG_RENDER_STAGE4_AOT_TARGET}")
message(STATUS "dng_render_stage4 metal target: ${DNG_RENDER_STAGE4_USE_METAL_TARGET}")
message(STATUS "dng_render_stage4 matrix strict expr: ${DNG_RENDER_STAGE4_MATRIX_STRICT_EXPR}")
message(STATUS "dng_render_stage4 encode truncate: ${DNG_RENDER_STAGE4_ENCODE_TRUNCATE}")
message(STATUS "dng_render_stage4 matrix assoc alt: ${DNG_RENDER_STAGE4_MATRIX_ASSOC_ALT}")
message(STATUS "dng_render_stage4 matrix strict R/G/B: ${DNG_RENDER_STAGE4_MATRIX_STRICT_R}/${DNG_RENDER_STAGE4_MATRIX_STRICT_G}/${DNG_RENDER_STAGE4_MATRIX_STRICT_B}")
message(STATUS "dng_render_stage4 abc assoc alt: ${DNG_RENDER_STAGE4_ABC_ASSOC_ALT}")
message(STATUS "dng_render_stage4 frontend split abc/tone: ${DNG_RENDER_STAGE4_FRONTEND_SPLIT_ABC}/${DNG_RENDER_STAGE4_FRONTEND_SPLIT_TONE}")
message(STATUS "dng_render_stage4 split muladd abc/final: ${DNG_RENDER_STAGE4_ABC_SPLIT_MULADD}/${DNG_RENDER_STAGE4_MATRIX_SPLIT_MULADD}")
message(STATUS "dng_render_stage4 no vectorize: ${DNG_RENDER_STAGE4_NO_VECTORIZE}")
message(STATUS "dng_render_tail strict float: ${DNG_RENDER_TAIL_STRICT_FLOAT}")
message(STATUS "dng_render_tail strict R/G/B: ${DNG_RENDER_TAIL_STRICT_R}/${DNG_RENDER_TAIL_STRICT_G}/${DNG_RENDER_TAIL_STRICT_B}")

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/dng_pipeline.h
    COMMAND dng_generator -g dng_pipeline -f dng_pipeline -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_generator
    COMMENT "Generating Halide AOT Pipeline..."
)
add_custom_target(dng_halide_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_pipeline.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.h
    COMMAND dng_warp_generator -g rectilinear_warp -f rectilinear_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_warp_generator
    COMMENT "Generating Halide AOT Rectilinear Warp..."
)
add_custom_target(dng_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.h
    COMMAND dng_render_generator -g dng_render_stage4 -f dng_render_stage4 -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render..."
)
add_custom_target(dng_render_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.h
    COMMAND dng_render_nomap_generator -g dng_render_nomap_stage4 -f dng_render_nomap_stage4 -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_render_nomap_generator
    COMMENT "Generating Halide AOT Stage4 No-Map Render..."
)
add_custom_target(dng_render_nomap_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.h
    COMMAND dng_render_maps_noencode_generator -g dng_render_maps_noencode_stage4 -f dng_render_maps_noencode_stage4 -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_render_maps_noencode_generator
    COMMENT "Generating Halide AOT Stage4 Maps-NoEncoding Render..."
)
add_custom_target(dng_render_maps_noencode_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.h
    COMMAND dng_render_tail_generator -g dng_render_tail_stage4 -f dng_render_tail_stage4 -o ${HALIDE_OUTPUT_DIR} target=$<IF:$<BOOL:${DNG_RENDER_TAIL_STRICT_FLOAT}>,${AOT_TARGET}-strict_float-no_runtime,${AOT_TARGET}-no_runtime>
    DEPENDS dng_render_tail_generator
    COMMENT "Generating Halide AOT Stage4 Tail Render..."
)
add_custom_target(dng_render_tail_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.a)

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.h
    COMMAND dng_render_tonetail_generator -g dng_render_tonetail_stage4 -f dng_render_tonetail_stage4 -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_render_tonetail_generator
    COMMENT "Generating Halide AOT Stage4 Tone+Tail Render..."
)
add_custom_target(dng_render_tonetail_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.a)

target_include_directories(dng_decoder_native PUBLIC ${HALIDE_OUTPUT_DIR})
add_dependencies(dng_decoder_native dng_halide_aot_target)
add_dependencies(dng_decoder_native dng_warp_aot_target)
add_dependencies(dng_decoder_native dng_render_aot_target)
add_dependencies(dng_decoder_native dng_render_nomap_aot_target)
add_dependencies(dng_decoder_native dng_render_maps_noencode_aot_target)
add_dependencies(dng_decoder_native dng_render_tail_aot_target)
add_dependencies(dng_decoder_native dng_render_tonetail_aot_target)

# Halide setup — We only need Halide::Runtime for AOT, but Halide::Halide is fine
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)
target_link_libraries(dng_decoder_native Halide::Halide ${HALIDE_OUTPUT_DIR}/dng_pipeline.a ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.a)

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


# Test application
add_executable(test_dng_decoder tests/test_main.cpp)
target_include_directories(test_dng_decoder PRIVATE ${INC_DIR})
target_link_libraries(test_dng_decoder dng_decoder_native Halide::Halide)

# Color accuracy / visual regression test
add_executable(test_color_accuracy tests/test_color_accuracy.cpp)
target_include_directories(test_color_accuracy PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_color_accuracy dng_sdk)
if(APPLE)
    target_link_libraries(test_color_accuracy ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# Color Diagnostic Tool
add_executable(test_color_diagnostic tests/test_color_diagnostic.cpp)
target_include_directories(test_color_diagnostic PRIVATE ${INC_DIR} ${DNG_SDK_DIR} ${HALIDE_DIR}/include)
target_link_libraries(test_color_diagnostic dng_decoder_native dng_sdk Halide::Halide)
if(APPLE)
    target_link_libraries(test_color_diagnostic ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# Phase 6.5: Edge Cases Test
add_executable(test_edge_cases tests/test_edge_cases.cpp)
target_include_directories(test_edge_cases PRIVATE ${INC_DIR})
target_link_libraries(test_edge_cases dng_decoder_native Halide::Halide)

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
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.a ${TURBOJPEG_LIBRARY})
else()
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a ${HALIDE_OUTPUT_DIR}/dng_render_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_nomap_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_maps_noencode_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tail_stage4.a ${HALIDE_OUTPUT_DIR}/dng_render_tonetail_stage4.a ${JPEG_LIBRARIES})
endif()
add_dependencies(test_decode dng_warp_aot_target)
add_dependencies(test_decode dng_render_aot_target)
add_dependencies(test_decode dng_render_nomap_aot_target)
add_dependencies(test_decode dng_render_maps_noencode_aot_target)
add_dependencies(test_decode dng_render_tail_aot_target)
add_dependencies(test_decode dng_render_tonetail_aot_target)
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
    ${HALIDE_DIR}/include)
target_link_libraries(test_mosaic_halide dng_sdk Halide::Halide)
if(APPLE)
    target_link_libraries(test_mosaic_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# Debug demosaic test
add_executable(test_mosaic_debug tests/test_mosaic_debug.cpp
    dng_sdk_custom/source/dng_mosaic_halide.cpp)
target_include_directories(test_mosaic_debug PRIVATE
    ${INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_mosaic_debug dng_sdk Halide::Halide)
if(APPLE)
    target_link_libraries(test_mosaic_debug ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
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

add_executable(test_warp_rectilinear_halide
    tests/test_warp_rectilinear_halide.cpp
    dng_sdk_custom/source/dng_mosaic_halide.cpp
    dng_sdk_custom/source/dng_warp_halide.cpp)
target_include_directories(test_warp_rectilinear_halide PRIVATE
    ${INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_custom/source
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_warp_rectilinear_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/rectilinear_warp.a)
add_dependencies(test_warp_rectilinear_halide dng_warp_aot_target)
if(APPLE)
    target_link_libraries(test_warp_rectilinear_halide
        ${COREFOUNDATION_LIBRARY}
        ${CORESERVICES_LIBRARY}
        ${METAL_LIBRARY}
        ${FOUNDATION_LIBRARY})
endif()
