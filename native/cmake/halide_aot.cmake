# halide_aot.cmake - AOT target strings and the AOT custom commands
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 571-821)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
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
    elseif(WIN32)
        # W6a (2026-08-21, Windows port): same strict Vulkan device contract as
        # Android, x86-64 host. The pipeline has no CPU fallback route
        # (dng_pipeline.cpp requireGpuBackend), so a GPU feature is mandatory
        # rather than optional here.
        set(AOT_TARGET "x86-64-windows-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query")
    else()
        set(AOT_TARGET "host-no_asserts-no_bounds_query")
    endif()
endif()
message(STATUS "Halide AOT target: ${AOT_TARGET}")

# W6b (2026-08-21, Windows port, risk R8): Halide names AOT static-library
# outputs after the *target* platform, not the host — `.a` on POSIX and `.lib`
# for windows targets (see
# third_party/halide/lib/cmake/HalideHelpers/HalideGeneratorHelpers.cmake).
# Every AOT artifact path below therefore goes through this variable; a literal
# ".a" left anywhere in the AOT/link blocks is a missing-file link error on
# Windows whose message points at the build directory, not at this file.
if(AOT_TARGET MATCHES "windows")
    set(DNG_AOT_LIB_EXT ".lib")
else()
    set(DNG_AOT_LIB_EXT ".a")
endif()

# W7 (2026-08-21, Windows port, risk R2): the three-channel-split Stage4 kernel
# (dng_render_stage4_split) exists to dodge the Halide v21 SPIR-V Tuple R==G
# bug — a *Vulkan* codegen defect, not an Android platform trait. Select it by
# the AOT target's backend instead of by platform, so Windows-Vulkan inherits
# the same workaround. Android is unaffected: both its presets resolve
# AOT_TARGET to a string containing "vulkan", exactly the cases the previous
# `ANDROID OR override MATCHES "android"` condition covered.
if(AOT_TARGET MATCHES "vulkan")
    set(DNG_STAGE4_SPLIT_KERNEL ON)
else()
    set(DNG_STAGE4_SPLIT_KERNEL OFF)
endif()
message(STATUS "Halide AOT lib extension: ${DNG_AOT_LIB_EXT}; Stage4 split kernel: ${DNG_STAGE4_SPLIT_KERNEL}")

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
    OUTPUT ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    COMMAND dng_demosaic_generator -r halide_runtime -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_demosaic_generator
    COMMENT "Generating standalone Halide runtime..."
)
add_custom_target(halide_runtime_target DEPENDS ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp.h
    COMMAND rectilinear_warp_generator -g rectilinear_warp -f rectilinear_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS rectilinear_warp_generator
    COMMENT "Generating Halide AOT Rectilinear Warp..."
)
add_custom_target(dng_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.h
    COMMAND dng_demosaic_generator -g dng_demosaic_bilinear -f dng_demosaic_bilinear -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_demosaic_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic..."
)
add_custom_target(dng_demosaic_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT})

# P17 T9: generic-RAW fused normalize + Bayer demosaic AOT kernel.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic.h
    COMMAND raw_bayer_demosaic_generator -g raw_bayer_demosaic -f raw_bayer_demosaic
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_bayer_demosaic_generator
    COMMENT "Generating Halide AOT generic Bayer normalize+demosaic..."
)
add_custom_target(raw_bayer_demosaic_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT})

# P17 T11: generic-RAW fused normalize + X-Trans demosaic AOT kernel.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic.h
    COMMAND raw_xtrans_demosaic_generator -g raw_xtrans_demosaic -f raw_xtrans_demosaic
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_xtrans_demosaic_generator
    COMMENT "Generating Halide AOT generic X-Trans normalize+demosaic..."
)
add_custom_target(raw_xtrans_demosaic_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})

# P19 T7: generic-RAW linear-RGB normalize-only AOT kernel (no demosaic).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize.h
    COMMAND raw_linear_rgb_normalize_generator -g raw_linear_rgb_normalize -f raw_linear_rgb_normalize
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_linear_rgb_normalize_generator
    COMMENT "Generating Halide AOT generic linear-RGB normalize..."
)
add_custom_target(raw_linear_rgb_normalize_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.h
    COMMAND dng_demosaic_warp_generator -g dng_demosaic_warp -f dng_demosaic_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime$<$<BOOL:${DNG_DEMOSAIC_WARP_STRICT_FLOAT}>:-strict_float> fast_codegen=$<IF:$<BOOL:${DNG_DEMOSAIC_WARP_FAST_CODEGEN}>,true,false>
    DEPENDS dng_demosaic_warp_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic+Warp..."
)
add_custom_target(dng_demosaic_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4.h
    COMMAND dng_render_generator -g dng_render_stage4 -f dng_render_stage4 -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render..."
)
add_custom_target(dng_render_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT})

# Sized decode (targetWidth): box-filter-downscaling Stage4 variant. Emitted
# from the SAME generator binary via -g/-f, exactly like the Android variant
# below, so that dng_render_stage4 itself stays byte-identical (its output SHAs
# are pinned gate artifacts — Gotcha #99).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled.h
    COMMAND dng_render_generator -g dng_render_stage4_scaled -f dng_render_stage4_scaled -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render (box-filter scaled)..."
)
add_custom_target(dng_render_scaled_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT})

# Variant A of the sized kernel: box-averages the Stage3 source BEFORE the
# colour math (the variant above averages after it). The two co-exist on
# purpose so the averaging-order trade-off can be measured side by side on a
# real photograph; see the class comments in DngRenderGenerator.cpp.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg.h
    COMMAND dng_render_generator -g dng_render_stage4_scaled_preavg -f dng_render_stage4_scaled_preavg -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render (box-filter scaled, pre-average)..."
)
add_custom_target(dng_render_scaled_preavg_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})

# Phase 14: Vulkan three-channel-split Stage4 generator (was Android-only; see
# the DNG_STAGE4_SPLIT_KERNEL note above).
# Eliminates Tuple + dim(2) codegen that triggers Halide v21 SPIR-V R==G bug.
if(DNG_STAGE4_SPLIT_KERNEL)
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split.h
        COMMAND dng_render_generator -g dng_render_stage4_split -f dng_render_stage4_split -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET} diag_stage=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render (Android 3-channel split)..."
    )
    add_custom_target(dng_render_android_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})

    # P14-W4-4 GO/NO-GO probe: isolated interleaved flat-1D src-read AOT.
    # Separate kernel/signature; does not touch the production Stage4 kernel.
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe.h
        COMMAND dng_render_generator -g dng_render_stage4_split_probe -f dng_render_stage4_split_probe -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render PROBE (Android interleaved-src go/no-go)..."
    )
    add_custom_target(dng_render_android_probe_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe${DNG_AOT_LIB_EXT})
endif()

# Phase 10 Sprint C1: MapPolynomial AOT (Stage 2 OpcodeList2 GPU).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.h
    COMMAND dng_opcode_polynomial_generator -g dng_opcode_polynomial
        -f dng_opcode_polynomial -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.h
    COMMAND dng_opcode_polynomial3_generator -g dng_opcode_polynomial3
        -f dng_opcode_polynomial3 -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial3_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 batched MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial3_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})

# When building host generators only (Stage 1), pull AOT targets into ALL
# so `cmake --build` actually runs the generators.
if(DNG_HOST_GENERATORS_ONLY)
    set(_DNG_ALL_AOT_DEPS
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
    )
    if(DNG_STAGE4_SPLIT_KERNEL)
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe${DNG_AOT_LIB_EXT})
    endif()
    add_custom_target(dng_all_aot ALL DEPENDS ${_DNG_ALL_AOT_DEPS})
endif()

endif() # NOT DNG_CROSS_BUILD (AOT custom commands)
