#!/usr/bin/env bash
# Task 8 / gate G-C -- re-gate the PRODUCTION split kernel (dng_render_stage4_split,
# class DngRenderStage4Android) on real Android hardware after T7 replaced the
# 8-way select chain with branch-free flag arithmetic (commit a396f9b).
#
# Differences from the Task 6 harness, all deliberate:
#   * the kernel under test is the PRODUCTION generator, not the V3 experimental
#     copy -- that is the point of this gate;
#   * the production argument order differs (crop_l/crop_t precede src_scale and
#     orientation/unoriented_* follow it), so the harness has its own invoke();
#   * provenance is asserted on the GENERATED HEADER: the split class only gained
#     the orientation/unoriented_width/unoriented_height scalars in T7, so a
#     header carrying all three proves the generator binary contains post-T7 code
#     without appealing to mtime.
#
# G-4: the Vulkan target keeps vk_int8-vk_int16-vk_int64.
# G-10: real hardware only; MoltenVK is not a gate for this u8-output kernel.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
HALIDE="${REPO}/native/third_party/halide"
GEN="${REPO}/native/build/dng_render_generator"
OUT="${HERE}/build_t8"
NDK="${NDK_ROOT:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}"
API=26

VK_TARGET="arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query"
CPU_TARGET="arm-64-android"

rm -rf "${OUT}"; mkdir -p "${OUT}/aot"
echo "GENERATOR=${GEN}"
echo "VK_TARGET=${VK_TARGET}"

"${GEN}" -g dng_render_stage4_split -f k_vk_gt_on -n k_vk_gt_on -o "${OUT}/aot" \
    -e static_library,c_header target="${VK_TARGET}-no_runtime" guard_tail=true
"${GEN}" -g dng_render_stage4_split -f k_vk_gt_off -n k_vk_gt_off -o "${OUT}/aot" \
    -e static_library,c_header target="${VK_TARGET}-no_runtime" guard_tail=false
"${GEN}" -g dng_render_stage4_split -f k_ref_cpu -n k_ref_cpu -o "${OUT}/aot" \
    -e static_library,c_header target="${CPU_TARGET}-no_runtime" guard_tail=true
"${GEN}" -r halide_runtime_vk -o "${OUT}/aot" -e static_library target="${VK_TARGET}"

# --- provenance: the generated header must carry the T7 orientation scalars ---
PROV="${REPO}/tmp/verify/orient_prod_t8_generator_provenance.txt"
{
  echo "=== T8 GENERATOR PROVENANCE (content marker, not mtime) ==="
  echo "GENERATOR_PATH=${GEN}"
  echo "GENERATOR_SHA256=$(shasum -a 256 "${GEN}" | awk '{print $1}')"
  echo "-- generated header signature for k_vk_gt_on --"
  grep -o 'int k_vk_gt_on([^;]*' "${OUT}/aot/k_vk_gt_on.h" | head -1
  echo "-- required post-T7 scalars present in the generated header? --"
  for s in _orientation _unoriented_width _unoriented_height; do
    echo "MARKER ${s} count=$(grep -c -- "${s}" "${OUT}/aot/k_vk_gt_on.h" || true)"
  done
} > "${PROV}"
PROV_RC=$?
echo "PROV_RC=${PROV_RC}" >> "${PROV}"
for s in _orientation _unoriented_width _unoriented_height; do
  grep -q -- "${s}" "${OUT}/aot/k_vk_gt_on.h" || {
      echo "FATAL: generated header lacks ${s} -- generator predates T7"; exit 9; }
done

# guard_tail positive control
{
  echo "=== GUARD_TAIL ARCHIVE CONTROL ==="
  shasum -a 256 "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a"
  ls -l "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a" | awk '{print $5, $NF}'
} >> "${PROV}"

# Oracle TU: still at native/src/ffi/ceyx_orient.cpp (T9 has not yet moved it
# to native/tests/oracle/). Update this path when T9 lands.
CXX="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android${API}-clang++"
"${CXX}" -std=c++17 -O2 -fPIE -pie \
    -I"${HALIDE}/include" -I"${HERE}/../v3" -I"${OUT}/aot" -I"${REPO}/native/include" \
    "${HERE}/test_orient_t8_device.cpp" \
    "${REPO}/native/src/ffi/ceyx_orient.cpp" \
    "${OUT}/aot/k_vk_gt_on.a" "${OUT}/aot/k_vk_gt_off.a" \
    "${OUT}/aot/k_ref_cpu.a" "${OUT}/aot/halide_runtime_vk.a" \
    -ldl -llog -o "${OUT}/test_orient_t8_device"

file "${OUT}/test_orient_t8_device"
echo "BUILD_OK"
