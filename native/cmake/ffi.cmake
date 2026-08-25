# ffi.cmake - AOT/platform link wiring of the shipped C-ABI dylib
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 823-936)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
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
    # P17 T9: src/raw_demosaic_reference.cpp (auto-globbed above) calls the
    # raw_bayer_demosaic AOT entry, so the dylib depends on and links it.
    add_dependencies(dng_decoder_native raw_bayer_demosaic_aot_target)
    # P17 T11: the same reference TU also calls the raw_xtrans_demosaic AOT
    # entry.
    add_dependencies(dng_decoder_native raw_xtrans_demosaic_aot_target)
    # P19 T7: generic-RAW linear-RGB normalize-only AOT kernel (Foveon X3F
    # pre-pass ahead of the shared Stage4 handoff).
    add_dependencies(dng_decoder_native raw_linear_rgb_normalize_aot_target)
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
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT})
# R2 sized decode: the pre-average scaled Stage4 kernel is dispatched by
# dng_render_halide.cpp on the non-split (macOS/Metal) branch only, so it is
# linked only there. The split branch refuses sized requests instead.
if(NOT DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(dng_decoder_native
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})
    if(NOT DNG_CROSS_BUILD)
        add_dependencies(dng_decoder_native dng_render_scaled_preavg_aot_target)
    endif()
endif()
# W7: link the split Stage4 kernel wherever it is generated (Vulkan targets).
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(dng_decoder_native
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
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
elseif(WIN32)
    # W8 (2026-08-21, Windows port): the Halide Vulkan runtime resolves its
    # entry points against the loader import library. The Vulkan SDK installer
    # sets VULKAN_SDK; -DDNG_VULKAN_LIB=<path to vulkan-1.lib> overrides it.
    set(DNG_VULKAN_LIB "" CACHE FILEPATH
        "Path to vulkan-1.lib (defaults to $ENV{VULKAN_SDK}/Lib/vulkan-1.lib)")
    if(NOT DNG_VULKAN_LIB)
        find_library(VULKAN_LIBRARY
            NAMES vulkan-1
            HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/Lib32")
        if(NOT VULKAN_LIBRARY)
            message(FATAL_ERROR
                "vulkan-1.lib not found. Install the LunarG Vulkan SDK (so that "
                "VULKAN_SDK is set) or pass -DDNG_VULKAN_LIB=<path/to/vulkan-1.lib>. "
                "There is no CPU fallback route in this pipeline.")
        endif()
    else()
        set(VULKAN_LIBRARY "${DNG_VULKAN_LIB}")
    endif()
    message(STATUS "Linking Vulkan loader: ${VULKAN_LIBRARY}")
    target_link_libraries(dng_decoder_native ${VULKAN_LIBRARY})
endif()

# Re-guard (split mechanics): the enclosing `if(NOT DNG_HOST_GENERATORS_ONLY)`
# opened above is closed here and re-opened at the top of tests.cmake, because
# CMake requires flow-control blocks to balance within one file. The condition
# is unchanged in between, so execution is identical to the monolith.
endif() # NOT DNG_HOST_GENERATORS_ONLY (continued in tests.cmake)
