#!/bin/bash
# Runs bin/prod_shape_probe.dart against the shipped dylib, steering the
# real DngNativeBindings.load() candidate search via DNG_NATIVE_BUILD_DIR
# (candidate 3) instead of the test-only libraryPath: override. See the
# coverage-limit comment at the top of prod_shape_probe.dart.
#
# Self-resolving: does not assume the caller's CWD. Propagates the probe's
# exit code as this script's exit code (0 = every assertion held).
#
# Any extra args (e.g. --expect=sized / --expect=fallback) are passed
# through verbatim to the probe; default is --expect=sized (see the probe's
# header comment).
#
# DNG_PROBE_LIB_DIR overrides which directory DNG_NATIVE_BUILD_DIR points
# at — defaults to $PKG_DIR/macos/Libraries (the shipped dylib). Set it to
# an alternate directory (e.g. <worktree>/tmp/old-dylib) to re-run AC4
# against a snapshot of the pre-R2 dylib after the shipped one has been
# replaced with a build that exports dng_decode_and_process_sized:
#   DNG_PROBE_LIB_DIR=<worktree>/tmp/old-dylib \
#     tool/run_prod_shape_probe.sh --expect=fallback
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$(dirname "$SCRIPT_DIR")"
WORKTREE_DIR="$(dirname "$PKG_DIR")"

export DNG_NATIVE_BUILD_DIR="${DNG_PROBE_LIB_DIR:-$PKG_DIR/macos/Libraries}"
SAMPLE="$WORKTREE_DIR/image_samples/lossless_dng_sample.dng"

cd "$PKG_DIR" || exit 1
dart run bin/prod_shape_probe.dart "$SAMPLE" "$@"
exit $?
