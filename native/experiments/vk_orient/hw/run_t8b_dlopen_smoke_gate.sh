#!/usr/bin/env bash
# T8b live-load proof on the real Adreno device. Appends to the refresh artifact.
#
# Promoted from native/scripts/tmp/t8b_dlopen_smoke.sh (gitignored scratch
# lane) per Round 3 cleanup task #15 item 2. The scratch version's bug:
# the trailing "verdict" block only wrote prose into the artifact file and
# then let the script fall off the end -- bash's own exit code was whatever
# the last `echo`/`>>` returned (always 0), regardless of what the two legs'
# DEVICE_RC actually were. Attempt 2's genuine control-leg failure (see the
# artifact's own "ATTEMPT 1 IS VOID" note) could not have turned this
# script red even if it had happened, because the verdict lived only in
# text a human had to read, never in the process exit status. Fixed here:
# both legs' DEVICE_RC are captured into shell variables (not just grepped
# from prose after the fact), the verdict is computed from those variables,
# printed as a single machine-parseable SMOKE_WRAPPER_VERDICT/_RC line, and
# `exit` uses that computed value as the script's own exit code.
#
# Two legs, both required:
#   COMPLETE   -- all three jniLibs .so pushed together; must LOAD (the claim).
#   INCOMPLETE -- only libdng_decoder_native.so pushed; must FAIL (the control).
# Without the second leg a green load is unfalsifiable: a program that always
# prints PASS looks identical to a working one. The control also shows WHICH
# dependency the loader names first, which is the under-reporting failure mode
# README-jnilibs.md:9-17 warns about.
#
# --dry-structural-test: skip adb entirely and exercise the RC-computation
# logic with stubbed leg exit codes (0 for leg1, nonzero for leg2, matching
# the expected-PASS shape) -- for local proof without a device attached.
# This proves the wrapper's arithmetic/branching is correct; it does NOT
# reproduce hardware behavior. Say so plainly in any report that cites it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
NDK="${NDK_ROOT:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}"
CXX="${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
ART="${REPO}/tmp/verify/orient_prod_t8b_so_refresh.txt"
BIN="${HERE}/t8b_dlopen_smoke"
ABI="${REPO}/plugin/android/src/main/jniLibs/arm64-v8a"
DEV_OK=/data/local/tmp/ceyx_t8b_dlopen_complete
DEV_BAD=/data/local/tmp/ceyx_t8b_dlopen_incomplete

DRY=0
if [ "${1:-}" = "--dry-structural-test" ]; then
  DRY=1
fi

mkdir -p "${REPO}/tmp/verify"

if [ "${DRY}" -eq 1 ]; then
  echo "DRY_STRUCTURAL_TEST=1 (no adb/device involved -- stubbed leg exit codes only)"
  LEG1_RC=0
  LEG2_RC=1
else
  "${CXX}" -std=c++17 -O2 -fPIE -pie "${HERE}/t8b_dlopen_smoke.cpp" \
      -ldl -o "${BIN}"
  BUILD_RC=$?

  {
    echo
    echo "=== LIVE-LOAD PROOF (dlopen on real device, RTLD_NOW), harness fixed (RC bug) ==="
    echo "DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "SMOKE_BUILD_RC=${BUILD_RC}"
    echo "ADB_DEVICES:"
    adb devices
    echo "DEVICE_MODEL=$(adb shell getprop ro.product.model | tr -d '\r')"
  } >> "${ART}"

  if [ "${BUILD_RC}" -ne 0 ]; then
    echo "SMOKE_ABORT=build_failed" >> "${ART}"
    echo "SMOKE_ABORT=build_failed"
    exit 1
  fi

  # ---- Leg 1: COMPLETE set (the claim; must load, DEVICE_RC=0) ----
  adb shell "rm -rf ${DEV_OK}; mkdir -p ${DEV_OK}"
  adb push "${ABI}/libdng_decoder_native.so" "${DEV_OK}/" >/dev/null
  adb push "${ABI}/libheif.so"               "${DEV_OK}/" >/dev/null
  adb push "${ABI}/libde265.so"              "${DEV_OK}/" >/dev/null
  adb push "${BIN}"                          "${DEV_OK}/" >/dev/null
  adb shell "chmod 755 ${DEV_OK}/t8b_dlopen_smoke"

  LEG1_OUT="$(adb shell "cd ${DEV_OK} && LD_LIBRARY_PATH=${DEV_OK} ./t8b_dlopen_smoke ${DEV_OK}/libdng_decoder_native.so 2>&1; echo DEVICE_RC=\$?")"
  LEG1_RC="$(printf '%s\n' "${LEG1_OUT}" | grep -o 'DEVICE_RC=[0-9]*' | tail -n 1 | cut -d= -f2)"
  LEG1_RC="${LEG1_RC:-127}"  # 127: DEVICE_RC line never appeared -- treat as failure, not as "no news is good news"

  {
    echo
    echo "--- LEG 1: COMPLETE SET (expect load OK) ---"
    echo "DEVICE_DIR=${DEV_OK}"
    echo "FILES_PUSHED:"
    adb shell "ls -l ${DEV_OK}"
    echo "ON_DEVICE_MD5:"
    adb shell "md5sum ${DEV_OK}/libdng_decoder_native.so ${DEV_OK}/libheif.so ${DEV_OK}/libde265.so"
    echo "RUN_OUTPUT:"
    printf '%s\n' "${LEG1_OUT}"
    echo "LEG1_CAPTURED_DEVICE_RC=${LEG1_RC}"
  } >> "${ART}" 2>&1

  # ---- Leg 2: INCOMPLETE set (the control -- must fail, DEVICE_RC != 0) ----
  adb shell "rm -rf ${DEV_BAD}; mkdir -p ${DEV_BAD}"
  adb push "${ABI}/libdng_decoder_native.so" "${DEV_BAD}/" >/dev/null
  adb push "${BIN}"                          "${DEV_BAD}/" >/dev/null
  adb shell "chmod 755 ${DEV_BAD}/t8b_dlopen_smoke"

  LEG2_OUT="$(adb shell "cd ${DEV_BAD} && LD_LIBRARY_PATH=${DEV_BAD} ./t8b_dlopen_smoke ${DEV_BAD}/libdng_decoder_native.so 2>&1; echo DEVICE_RC=\$?")"
  LEG2_RC="$(printf '%s\n' "${LEG2_OUT}" | grep -o 'DEVICE_RC=[0-9]*' | tail -n 1 | cut -d= -f2)"
  LEG2_RC="${LEG2_RC:-0}"  # 0: DEVICE_RC line never appeared -- for leg 2 that must ALSO count as a wrapper failure (see verdict below), so default to the value that fails to satisfy "must be nonzero"

  {
    echo
    echo "--- LEG 2: INCOMPLETE SET, codec libs withheld (expect load FAIL) ---"
    echo "DEVICE_DIR=${DEV_BAD}"
    echo "FILES_PUSHED:"
    adb shell "ls -l ${DEV_BAD}"
    echo "RUN_OUTPUT:"
    printf '%s\n' "${LEG2_OUT}"
    echo "LEG2_CAPTURED_DEVICE_RC=${LEG2_RC}"
  } >> "${ART}" 2>&1
fi

# ---- Verdict: computed from the captured leg RCs, not from artifact grep ----
if [ "${LEG1_RC}" -eq 0 ] && [ "${LEG2_RC}" -ne 0 ]; then
  WRAPPER_VERDICT=PASS
  WRAPPER_RC=0
else
  WRAPPER_VERDICT=FAIL
  WRAPPER_RC=1
fi

{
  echo
  echo "--- LIVE-LOAD VERDICT (RC computed from captured leg exit codes, not prose grep) ---"
  echo "LEG1_DEVICE_RC=${LEG1_RC} (expect 0)"
  echo "LEG2_DEVICE_RC=${LEG2_RC} (expect nonzero)"
  echo "SMOKE_WRAPPER_VERDICT=${WRAPPER_VERDICT}"
  echo "SMOKE_WRAPPER_RC=${WRAPPER_RC}"
} | if [ "${DRY}" -eq 1 ]; then cat; else tee -a "${ART}"; fi

echo "SMOKE_WRAPPER_VERDICT=${WRAPPER_VERDICT}"
echo "SMOKE_WRAPPER_RC=${WRAPPER_RC}"
exit "${WRAPPER_RC}"
