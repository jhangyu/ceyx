#!/bin/bash
# Re-fetch entry for the Halide v21.0.0 Vulkan runtime sources backing
# dng_processor/native/halide_runtime_fork/ (see that directory's README
# "Maintenance debt" section). Originally written for spike G1
# (source-level feasibility reading); reusable whenever a Halide version
# bump requires re-diffing upstream/.
# Vendored third_party/halide is a binary release (no src/runtime); VERSION file pins v21.0.0.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEST="$NATIVE_DIR/scripts/tmp/halide_v21_runtime_src"
mkdir -p "$DEST"
cd "$DEST" || exit 1
BASE=https://raw.githubusercontent.com/halide/Halide/v21.0.0/src/runtime
for f in vulkan.cpp vulkan_memory.h vulkan_resources.h vulkan_context.h vulkan_internal.h device_interface.cpp device_buffer_utils.h metal.m metal.cpp; do
    if curl -sfL -o "$f" "$BASE/$f"; then
        echo "OK $f ($(wc -l < "$f") lines)"
    else
        rm -f "$f"
        echo "MISS $f"
    fi
done
