#!/bin/bash
# Fetch the vendored Halide v21.0.0 binary distribution into
# dng_processor/native/third_party/halide/.
#
# Provenance (was tracked in third_party/halide/VERSION before the 2026-07-05
# history rewrite removed the ~540MB binary payload from git):
#   halide/Halide@v21.0.0
#   https://github.com/halide/Halide/releases/tag/v21.0.0
#   abi_notes: schedule changes break AOT artifacts; bump requires full regen.
#
# Run once after a fresh clone (CMake configure fails without it).
# The Vulkan runtime FORK sources are a separate concern — see
# halide_runtime_fork/scripts/fetch_halide_v21_runtime.sh.
set -eu
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="$NATIVE_DIR/third_party/halide"

ASSET="Halide-21.0.0-arm-64-osx-b629c80de18f1534ec71fddd8b567aa7027a0876.tar.gz"
URL="https://github.com/halide/Halide/releases/download/v21.0.0/$ASSET"

if [ -f "$DEST/lib/libHalide.a" ]; then
    echo "third_party/halide already present ($DEST) — nothing to do."
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "Downloading $ASSET ..."
curl -fL -o "$TMP/$ASSET" "$URL"
mkdir -p "$DEST"
tar -xzf "$TMP/$ASSET" -C "$DEST" --strip-components=1
printf 'halide/Halide@v21.0.0\nbinary_provenance: vendored from https://github.com/halide/Halide/releases/tag/v21.0.0\nabi_notes: schedule changes break AOT artifacts; bump requires full regen.\n' > "$DEST/VERSION"
echo "OK — $(du -sh "$DEST" | cut -f1) at $DEST"
