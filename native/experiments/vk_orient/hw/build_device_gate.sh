#!/usr/bin/env bash
# Task 6 (gate G-B) -- build the Android on-device sub-tile GuardWithIf gate.
#
# Produces ONE arm64 Android binary that links THREE Halide AOT pipelines:
#   k_ref_cpu     arm-64-android                (asserts + bounds query ON --
#                                                the reference must not be
#                                                subject to F-V3-3's CPU-branch
#                                                tail crash)
#   k_vk_gt_on    arm-64-android-vulkan-...     guard_tail=true
#   k_vk_gt_off   arm-64-android-vulkan-...     guard_tail=false
# plus ONE Halide runtime built for the Vulkan target. Every pipeline is
# emitted with -no_runtime so the single Vulkan runtime serves all three
# (standard Halide practice; the CPU pipeline needs no Vulkan symbols).
#
# G-4: the Vulkan target string keeps vk_int8-vk_int16-vk_int64. Removing
# vk_int8 is a hard codegen failure for a u8-output kernel.
# G-10: this script targets REAL ANDROID HARDWARE. MoltenVK is not a gate.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V3="${HERE}/.."/v3
REPO="$(cd "${HERE}/../../../.." && pwd)"
HALIDE="${REPO}/native/third_party/halide"
GEN="${V3}/build/v3_gen"
OUT="${HERE}/build"
NDK="${NDK_ROOT:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}"
API=26

VK_TARGET="arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query"
CPU_TARGET="arm-64-android"

rm -rf "${OUT}"
mkdir -p "${OUT}/aot"

echo "NDK=${NDK}"
echo "VK_TARGET=${VK_TARGET}"
echo "CPU_TARGET=${CPU_TARGET}"

# --- AOT codegen -----------------------------------------------------------
"${GEN}" -g dng_render_stage4_split_oriented -f k_vk_gt_on \
    -n k_vk_gt_on -o "${OUT}/aot" -e static_library,c_header \
    target="${VK_TARGET}-no_runtime" guard_tail=true
"${GEN}" -g dng_render_stage4_split_oriented -f k_vk_gt_off \
    -n k_vk_gt_off -o "${OUT}/aot" -e static_library,c_header \
    target="${VK_TARGET}-no_runtime" guard_tail=false
"${GEN}" -g dng_render_stage4_split_oriented -f k_ref_cpu \
    -n k_ref_cpu -o "${OUT}/aot" -e static_library,c_header \
    target="${CPU_TARGET}-no_runtime" guard_tail=true
"${GEN}" -r halide_runtime_vk -o "${OUT}/aot" -e static_library \
    target="${VK_TARGET}"

# guard_tail positive control: the two Vulkan archives must differ.
shasum -a 256 "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a"
ls -l "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a"

# --- cross compile the harness --------------------------------------------
CXX="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android${API}-clang++"
"${CXX}" -std=c++17 -O2 -fPIE -pie \
    -I"${HALIDE}/include" -I"${V3}" -I"${OUT}/aot" \
    -I"${REPO}/native/include" \
    "${HERE}/test_orient_vk_device.cpp" \
    "${REPO}/native/src/ffi/ceyx_orient.cpp" \
    "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a" \
    "${OUT}/aot/k_ref_cpu.a" "${OUT}/aot/halide_runtime_vk.a" \
    -ldl -llog -o "${OUT}/test_orient_vk_device"

file "${OUT}/test_orient_vk_device"
echo "BUILD_OK"
