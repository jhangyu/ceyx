#!/usr/bin/env bash
# Phase 10 cold-path fix verification — lossless persistent repeat=3 control
# (Bayer path should be unaffected; no [MapPolynomialTiming] lines expected).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"

export DNG_TEST_DECODE_REPEAT=3
export DNG_STAGE2_SDK_TIMING=1
export DNG_MAP_POLY_TIMING=1

exec "${REPO_ROOT}/dng_processor/native/build/test_decode" \
    "${REPO_ROOT}/image_samples/lossless_dng_sample.dng" test auto halide-metal
