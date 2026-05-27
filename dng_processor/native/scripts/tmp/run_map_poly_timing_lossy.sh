#!/usr/bin/env bash
# Task #1 (lead2) — lossy MapPolynomial cold-path subdivision timing.
# All env vars exported inside the script (no inline env prefix).
set -euo pipefail

export DNG_TEST_DECODE_REPEAT=3
export DNG_STAGE2_SDK_TIMING=1
export DNG_MAP_POLY_TIMING=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../.."

exec "${REPO_ROOT}/dng_processor/native/build/test_decode" \
    "${REPO_ROOT}/image_samples/lossy_dng_sample.dng" \
    test auto halide-metal
