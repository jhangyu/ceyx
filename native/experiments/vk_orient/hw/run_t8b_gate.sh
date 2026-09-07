#!/usr/bin/env bash
# Task 8b / gate G-C -- deploy to a real Android device and gate the THIRD
# orientation formulation (T7b host-computed affine coefficients, commit
# bac3cbe): run the gate THREE times into ONE artifact, then pull the ON-DEVICE
# binary BACK and prove by symbol table that what ran contains the kernels under
# test.
#
# G-10: this is the ONLY admissible gate for the u8-output Vulkan kernel.
#       MoltenVK results are inadmissible and are not produced here.
# G-13: predictions are pre-registered in tmp/verify/orient_prod_t8b_predictions.txt
#       BEFORE this runs; this script refuses to start if that file is absent.
# G-9c: `nm` output goes to a FILE and is grepped from the file -- never
#       `nm | grep -q`, which returns 141 under pipefail when the symbol IS
#       found (grep exits early, nm takes SIGPIPE) and so inverts the check.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
NDK="${NDK_ROOT:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}"
NM="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"

BIN="${HERE}/build_t8b/test_orient_t8b_device"
DEV_DIR=/data/local/tmp/ceyx_orient_gate_t8b
ART="${REPO}/tmp/verify/orient_prod_t8b_device_decode.txt"
SYMS="${REPO}/tmp/verify/orient_prod_t8b_deployed_syms.txt"
PRED="${REPO}/tmp/verify/orient_prod_t8b_predictions.txt"

mkdir -p "${REPO}/tmp/verify"

# G-13 interlock: no run without a pre-registered prediction file.
[ -f "${PRED}" ] || { echo "FATAL: predictions not pre-registered at ${PRED}"; exit 13; }

# Truncate: one run of this script == one artifact, no accretion across attempts.
: > "${ART}"

{
  echo "=== RUN HEADER ==="
  echo "GATE=G-C TASK=8b FORMULATION=affine_host_coeffs"
  echo "WINDOW_OPEN=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "ARGV_BUILD=bash ${HERE}/build_t8b_gate.sh"
  echo "ARGV_RUN=bash ${HERE}/run_t8b_gate.sh"
  echo "GIT_HEAD=$(git -C "${REPO}" rev-parse HEAD)"
  echo "PREDICTIONS_SHA256=$(shasum -a 256 "${PRED}" | awk '{print $1}')"
  echo "HOST_BIN_SHA256=$(shasum -a 256 "${BIN}" | awk '{print $1}')"
  echo "NDK=${NDK}"
  echo "ADB_DEVICES:"
  adb devices
  echo "DEVICE_MODEL=$(adb shell getprop ro.product.model | tr -d '\r')"
  echo "DEVICE_SOC=$(adb shell getprop ro.soc.model | tr -d '\r')"
  echo "ANDROID_RELEASE=$(adb shell getprop ro.build.version.release | tr -d '\r')"
  echo "GUARD_TAIL_ARCHIVE_CONTROL:"
  shasum -a 256 "${HERE}/build_t8b/aot/k_vk_gt_on.a" "${HERE}/build_t8b/aot/k_vk_gt_off.a"
  ls -l "${HERE}/build_t8b/aot/k_vk_gt_on.a" "${HERE}/build_t8b/aot/k_vk_gt_off.a" | awk '{print $5, $NF}'
} >> "${ART}"

adb shell "rm -rf ${DEV_DIR}; mkdir -p ${DEV_DIR}"
adb push "${BIN}" "${DEV_DIR}/test_orient_t8b_device" >/dev/null
adb shell "chmod 755 ${DEV_DIR}/test_orient_t8b_device"

# Vulkan loader shim (F-T6-5). The STOCK Halide v21 dist runtime resolves the
# loader by trying only "libvulkan.so.1" / "libvulkan.1.dylib". Android ships it
# as plain "/system/lib64/libvulkan.so" -- no .so.1 alias -- so without this
# symlink the stock runtime reports "Vulkan: Failed to resolve loader library
# methods" and the process dies before any kernel runs. HARNESS-ONLY: the
# shipped libdng_decoder_native.so already carries a plain "libvulkan.so" in its
# loader name list, so production does not depend on this shim.
adb shell "ln -sf /system/lib64/libvulkan.so ${DEV_DIR}/libvulkan.so.1"

# Deployment-layer-first (spec D4): prove the DEPLOYED artifact is the one we
# built and contains the kernels, BEFORE interpreting any red result.
adb shell "md5sum ${DEV_DIR}/test_orient_t8b_device" | tee -a "${ART}"
adb pull "${DEV_DIR}/test_orient_t8b_device" "${HERE}/build_t8b/pulled_from_device" >/dev/null
"${NM}" "${HERE}/build_t8b/pulled_from_device" > "${SYMS}" 2>&1 || true
"${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strings" \
    "${HERE}/build_t8b/pulled_from_device" > "${SYMS}.strings" 2>&1 || true
{
  echo "=== DEPLOYED SYMBOL PROOF (nm-to-file, then grep -- pulled BACK from device) ==="
  echo "SYMS_FILE=${SYMS}"
  echo "PULLED_SHA256=$(shasum -a 256 "${HERE}/build_t8b/pulled_from_device" | awk '{print $1}')"
  for sym in k_vk_gt_on k_vk_gt_off k_ref_cpu halide_vulkan_device_interface ceyx_orient_rgba; do
    c=$(grep -c "${sym}" "${SYMS}" || true)
    echo "SYM ${sym} count=${c}"
  done
  echo "-- T7b content markers in the DEPLOYED binary's strings --"
  for s in orient_a_x orient_b_x orient_c_x orient_a_y orient_b_y orient_c_y; do
    echo "DEPLOYED_MARKER ${s} count=$(grep -c -- "${s}" "${SYMS}.strings" || true)"
  done
  echo "-- retired T7 markers in the DEPLOYED binary's strings (expect 0) --"
  for s in unoriented_width unoriented_height; do
    echo "DEPLOYED_ANTIMARKER ${s} count=$(grep -c -- "${s}" "${SYMS}.strings" || true)"
  done
} >> "${ART}"

for i in 1 2 3; do
  {
    echo "=== RUN ${i}/3 ==="
    echo "RUN_START=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    adb shell "cd ${DEV_DIR} && LD_LIBRARY_PATH=${DEV_DIR} ./test_orient_t8b_device 2>&1; echo DEVICE_RC=\$?"
    echo "RUN_END=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >> "${ART}" 2>&1
done

{
  echo "=== ARTIFACT TAIL SELF-CAPTURE ==="
  echo "RUNS_RECORDED=$(grep -c '^=== RUN [123]/3 ===' "${ART}")"
  echo "DEVICE_RC_LINES=$(grep -c '^DEVICE_RC=' "${ART}")"
  echo "VERDICT_LINES=$(grep -c '^VERDICT=' "${ART}")"
  echo "WINDOW_CLOSE=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >> "${ART}"
RC=$?
echo "RC=${RC}" >> "${ART}"
echo "RC=${RC}"
