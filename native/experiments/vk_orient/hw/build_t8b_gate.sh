#!/usr/bin/env bash
# Task 8b / gate G-C -- re-gate the PRODUCTION split kernel
# (dng_render_stage4_split, class DngRenderStage4Android) on real Android
# hardware after T7b (commit bac3cbe) replaced the branch-free flag arithmetic
# with SIX HOST-COMPUTED AFFINE COEFFICIENTS. The kernel now evaluates only
#   ux = a_x*x + b_x*y + c_x ; uy = a_y*x + b_y*y + c_y
# with no select, no bool, no cast-from-bool and no orientation scalar at all.
#
# THIRD formulation. The first two both mis-lowered on Adreno 750 / Vulkan with
# 8/8-correct CPU controls on identical generator text:
#   F-T6-1  8-way select equality chain      -> 20/40 byte-compares mismatched
#   F-T8-1  branch-free three-flag form      -> 30/40 byte-compares mismatched
# See docs/logs/2026-09-07/Task_t6_android_device_gate.md and
#     docs/logs/2026-09-07/Task_t8_android_device_gate.md
#
# Differences from build_t8_gate.sh, all deliberate:
#   * provenance markers changed: the post-T7b generated header must carry
#     _orient_a_x/_orient_b_x/_orient_c_x/_orient_a_y/_orient_b_y/_orient_c_y
#     and must NOT carry the retired _orientation/_unoriented_* scalars. Both
#     directions are asserted, so a stale generator binary fails the build
#     rather than silently producing a T8-era archive. No appeal to mtime.
#   * artifacts are t8b_*, so the T8 RED evidence trail stays intact.
#
# G-4: the Vulkan target keeps vk_int8-vk_int16-vk_int64.
# G-10: real hardware only; MoltenVK is not a gate for this u8-output kernel.
# G-9c: symbol/marker greps read from a FILE, never `nm | grep -q` (which
#       returns 141 under pipefail when the symbol IS found).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
HALIDE="${REPO}/native/third_party/halide"
GEN="${REPO}/native/build/dng_render_generator"
OUT="${HERE}/build_t8b"
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

# --- provenance: the generated header must carry the T7b affine scalars and
# --- must NOT carry the retired T7 orientation scalars (both directions) ---
PROV="${REPO}/tmp/verify/orient_prod_t8b_generator_provenance.txt"
mkdir -p "${REPO}/tmp/verify"
{
  echo "=== T8b GENERATOR PROVENANCE (content marker, not mtime) ==="
  echo "DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "GENERATOR_PATH=${GEN}"
  echo "GENERATOR_SHA256=$(shasum -a 256 "${GEN}" | awk '{print $1}')"
  echo "GIT_HEAD=$(git -C "${REPO}" rev-parse HEAD)"
  echo "-- generated header signature for k_vk_gt_on --"
  grep -o 'int k_vk_gt_on([^;]*' "${OUT}/aot/k_vk_gt_on.h" | head -1
  echo "-- REQUIRED post-T7b affine scalars present in the generated header? --"
  for s in _orient_a_x _orient_b_x _orient_c_x _orient_a_y _orient_b_y _orient_c_y; do
    echo "MARKER ${s} count=$(grep -c -- "${s}" "${OUT}/aot/k_vk_gt_on.h" || true)"
  done
  echo "-- FORBIDDEN retired T7 scalars (must all be 0) --"
  for s in _unoriented_width _unoriented_height; do
    echo "ANTIMARKER ${s} count=$(grep -c -- "${s}" "${OUT}/aot/k_vk_gt_on.h" || true)"
  done
} > "${PROV}"

for s in _orient_a_x _orient_b_x _orient_c_x _orient_a_y _orient_b_y _orient_c_y; do
  grep -q -- "${s}" "${OUT}/aot/k_vk_gt_on.h" || {
      echo "FATAL: generated header lacks ${s} -- generator predates T7b (bac3cbe)"; exit 9; }
done
for s in _unoriented_width _unoriented_height; do
  if grep -q -- "${s}" "${OUT}/aot/k_vk_gt_on.h"; then
      echo "FATAL: generated header still carries ${s} -- generator is a T7-era build"; exit 9
  fi
done
echo "PROVENANCE_OK" >> "${PROV}"

# guard_tail positive control: the two archives must NOT be byte-identical,
# otherwise guard_tail=false is not actually a different build.
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
    -ldl -llog -o "${OUT}/test_orient_t8b_device"

file "${OUT}/test_orient_t8b_device"
echo "BUILD_OK"
