#!/usr/bin/env bash
# Phase 10 cold-path subdivision experiment — lossy sample, persistent repeat=3.
# All env vars are exported here (not prefixed on the command) to comply with
# CLAUDE.md subagent execution rules.
set -euo pipefail

export DNG_TEST_DECODE_REPEAT=3
export DNG_STAGE2_SDK_TIMING=1
export DNG_MAP_POLY_TIMING=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/../../../.."

exec "${REPO_ROOT}/dng_processor/native/build/test_decode" \
    "${REPO_ROOT}/image_samples/lossy_dng_sample.dng" \
    test auto halide-metal
