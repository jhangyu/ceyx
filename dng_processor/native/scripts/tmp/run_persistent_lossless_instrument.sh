#!/usr/bin/env bash
# Phase 10 cold-path subdivision experiment — lossless sample (control),
# persistent repeat=3. Lossless DNG has no MapPolynomial opcodes, so this
# is mainly a sanity check that the new logging does not fire on lossless.
set -euo pipefail

export DNG_TEST_DECODE_REPEAT=3
export DNG_STAGE2_SDK_TIMING=1
export DNG_MAP_POLY_TIMING=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../.."

exec "${REPO_ROOT}/dng_processor/native/build/test_decode" \
    "${REPO_ROOT}/image_samples/lossless_dng_sample.dng" \
    test auto halide-metal
