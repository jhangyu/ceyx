#!/usr/bin/env bash
# SUPERSEDED 2026-08-31 (R4 Task #8): this script now DELEGATES to the Python
# carrier, which is the official mainline for the Windows HEIF dist.
#
#   real implementation: native/scripts/deps/win_heif_dist.py
#   entry point:         python -m deps.win_heif_dist --dist <dir>
#
# The file is kept as a delegating shim rather than deleted so that anything
# still invoking it by path keeps working during the switchover. Deletion of
# the retired shell scripts is a separate, later step (D9) -- doing it here
# would conflate "the carrier works" with "the old path is gone", and only the
# first of those is proven by this round's CI.
#
# WHY THE PORT: every line this shim replaced ran under Git-Bash, where MSYS
# rewrites any argument that looks like a path, and where each `cmake` call was
# a shell command string rather than an argv list. The carrier invokes every
# tool through deps/run.py as an argv list with shell=False, so there is no
# shell left to reinterpret a drive letter, and it refuses to run under an
# MSYS/Cygwin interpreter at all.
#
# WHAT DID NOT CHANGE (all now asserted in Python, see win_heif_dist.py):
#   - decode-only: libde265 + libheif, WITH_KVAZAAR/WITH_AOM_* OFF, WITH_X265
#     OFF because x265 is GPL-2.0;
#   - libde265 is SELF-BUILT on Windows permanently -- manifest.toml
#     [component.libde265.source.windows] records three durable clang-cl
#     blockers against the vcpkg port;
#   - upstream's bin/libde265.dll + lib/de265.lib naming asymmetry is preserved
#     untidied, because heif.dll's import table names "libde265.dll";
#   - the .pins stamp format is byte-identical, so a dist built by the old
#     script is still recognised as current and is not needlessly rebuilt.
#
# LICENCE (do not "optimise" this away): libheif and libde265 are
# LGPL-3.0-or-later. They are built as SEPARATE SHARED LIBRARIES and linked
# dynamically, which satisfies LGPL-3 section 4(d)(1) outright -- a user can
# replace heif.dll / libde265.dll next to the application. Static linking into
# dng_decoder_native would trigger the 4(d)(0) duty to ship relinkable object
# files with every release.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST="${DNG_HEIF_DIST_OUT:-${NATIVE_DIR}/third_party/heif-dist-windows}"

echo "[heif-win] this script is a shim; delegating to the Python carrier"
echo "[heif-win]   python -m deps.win_heif_dist --dist ${DIST}"

# `python` here must be NATIVE Windows Python. If a Git-Bash/MSYS interpreter
# is picked up instead, deps/run.py rejects it explicitly by sys.platform
# rather than limping along under POSIX emulation -- that error is the correct
# outcome, not a regression to work around. CI does not rely on this shim at
# all: heif_dist_windows.yml invokes the carrier directly under `shell: pwsh`.
cd "${SCRIPT_DIR}"
# `set +e` around the call so the RC capture below actually executes: under
# errexit a non-zero python would abort the script first, and the log would
# then only ever be able to record RC=0 -- an exit status that reports success
# on every failure is worse than none at all.
set +e
python -m deps.win_heif_dist --dist "${DIST}"
RC=$?
set -e
echo "[heif-win] HEIF_DIST_WINDOWS_RC=${RC}"
exit "${RC}"
