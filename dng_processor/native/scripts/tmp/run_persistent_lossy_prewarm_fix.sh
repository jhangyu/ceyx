#!/usr/bin/env bash
# Phase 10 cold-path fix verification — lossy persistent repeat=3
# (Candidate A: prewarm lifetime coverage).
#
# Env vars exported here (NEVER prefix env on the command line; CLAUDE.md
# §Subagent rule #2).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"

export DNG_TEST_DECODE_REPEAT=3
export DNG_STAGE2_SDK_TIMING=1
export DNG_MAP_POLY_TIMING=1

exec "${REPO_ROOT}/dng_processor/native/build/test_decode" \
    "${REPO_ROOT}/image_samples/lossy_dng_sample.dng" test auto halide-metal
