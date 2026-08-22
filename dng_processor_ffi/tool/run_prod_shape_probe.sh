#!/bin/bash
# Runs bin/prod_shape_probe.dart against the shipped dylib, steering the
# real DngNativeBindings.load() candidate search via DNG_NATIVE_BUILD_DIR
# (candidate 3) instead of the test-only libraryPath: override. See the
# coverage-limit comment at the top of prod_shape_probe.dart.
#
# Self-resolving: does not assume the caller's CWD. Propagates the probe's
# exit code as this script's exit code (0 = every assertion held).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$(dirname "$SCRIPT_DIR")"
WORKTREE_DIR="$(dirname "$PKG_DIR")"

export DNG_NATIVE_BUILD_DIR="$PKG_DIR/macos/Libraries"
SAMPLE="$WORKTREE_DIR/image_samples/lossless_dng_sample.dng"

cd "$PKG_DIR" || exit 1
dart run bin/prod_shape_probe.dart "$SAMPLE"
exit $?
