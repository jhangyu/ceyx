#!/usr/bin/env bash
# Task 8 / gate G-C -- deploy to a real Android device (T6 runner, retargeted
# at the PRODUCTION dng_render_stage4_split kernel after the T7 flag redesign), run the gate THREE
# times into ONE artifact, then pull the ON-DEVICE binary back and prove by
# symbol table that what ran contains the kernels under test.
#
# G-10: this is the ONLY admissible gate for the u8-output Vulkan kernel.
#       MoltenVK results are inadmissible and are not produced here.
# G-13: predictions are pre-registered in tmp/verify/ BEFORE this runs.
# G-9c: `nm` output goes to a FILE and is grepped from the file -- never
#       `nm | grep -q`, which returns 141 under pipefail when the symbol IS
#       found (grep exits early, nm takes SIGPIPE) and so inverts the check.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
NDK="${NDK_ROOT:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}"
NM="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"

BIN="${HERE}/build_t8/test_orient_t8_device"
DEV_DIR=/data/local/tmp/ceyx_orient_gate_t8
ART="${REPO}/tmp/verify/orient_prod_t8_device_decode.txt"
SYMS="${REPO}/tmp/verify/orient_prod_t8_deployed_syms.txt"

mkdir -p "${REPO}/tmp/verify"

{
  echo "=== RUN HEADER ==="
  echo "DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "HOST_BIN_SHA256=$(shasum -a 256 "${BIN}" | awk '{print $1}')"
  echo "NDK=${NDK}"
  echo "ADB_DEVICES:"
  adb devices
  echo "DEVICE_MODEL=$(adb shell getprop ro.product.model | tr -d '\r')"
  echo "DEVICE_SOC=$(adb shell getprop ro.soc.model | tr -d '\r')"
  echo "ANDROID_RELEASE=$(adb shell getprop ro.build.version.release | tr -d '\r')"
  echo "GUARD_TAIL_ARCHIVE_CONTROL:"
  shasum -a 256 "${HERE}/build_t8/aot/k_vk_gt_on.a" "${HERE}/build_t8/aot/k_vk_gt_off.a"
  ls -l "${HERE}/build_t8/aot/k_vk_gt_on.a" "${HERE}/build_t8/aot/k_vk_gt_off.a" | awk '{print $5, $NF}'
} >> "${ART}"

adb shell "rm -rf ${DEV_DIR}; mkdir -p ${DEV_DIR}"
adb push "${BIN}" "${DEV_DIR}/test_orient_t8_device" >/dev/null
adb shell "chmod 755 ${DEV_DIR}/test_orient_t8_device"

# Vulkan loader shim. The STOCK Halide v21 dist runtime this experiment links
# resolves the loader by trying only "libvulkan.so.1" / "libvulkan.1.dylib"
# (native/third_party/halide runtime; same list as
# native/halide_runtime_fork/upstream/vulkan_interface.h:50-56). Android ships
# the loader as plain "/system/lib64/libvulkan.so" -- no .so.1 alias -- so the
# stock runtime reports "Vulkan: Failed to resolve loader library methods" and
# the process dies before any kernel runs. A local symlink plus LD_LIBRARY_PATH
# makes dlopen("libvulkan.so.1") resolve. This is a HARNESS concern only: the
# SHIPPED plugin/android/.../libdng_decoder_native.so already carries a plain
# "libvulkan.so" string in its loader name list, i.e. production does not
# depend on this shim.
adb shell "ln -sf /system/lib64/libvulkan.so ${DEV_DIR}/libvulkan.so.1"

# Deployment-layer-first (spec D4): prove the DEPLOYED artifact is the one we
# built and contains the kernels, BEFORE interpreting any red result.
adb shell "md5sum ${DEV_DIR}/test_orient_t8_device" | tee -a "${ART}"
adb pull "${DEV_DIR}/test_orient_t8_device" "${HERE}/build_t8/pulled_from_device" >/dev/null
"${NM}" "${HERE}/build_t8/pulled_from_device" > "${SYMS}" 2>&1 || true
{
  echo "=== DEPLOYED SYMBOL PROOF (nm-to-file, then grep) ==="
  echo "SYMS_FILE=${SYMS}"
  echo "PULLED_SHA256=$(shasum -a 256 "${HERE}/build_t8/pulled_from_device" | awk '{print $1}')"
  for sym in k_vk_gt_on k_vk_gt_off k_ref_cpu halide_vulkan_device_interface ceyx_orient_rgba; do
    c=$(grep -c "${sym}" "${SYMS}" || true)
    echo "SYM ${sym} count=${c}"
  done
} >> "${ART}"

for i in 1 2 3; do
  {
    echo "=== RUN ${i}/3 ==="
    adb shell "cd ${DEV_DIR} && LD_LIBRARY_PATH=${DEV_DIR} ./test_orient_t8_device 2>&1; echo DEVICE_RC=\$?"
  } >> "${ART}" 2>&1
done

{
  echo "=== ARTIFACT TAIL SELF-CAPTURE ==="
  echo "RUNS_RECORDED=$(grep -c '^=== RUN [123]/3 ===' "${ART}")"
} >> "${ART}"
RC=$?
echo "RC=${RC}" >> "${ART}"
echo "RC=${RC}"
