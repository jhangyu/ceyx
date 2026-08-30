#!/usr/bin/env bash
# Builds the WINDOWS libheif + libde265 distribution: DECODE-ONLY and
# DYNAMICALLY LINKED, into native/third_party/heif-dist-windows/.
#
# Sibling of fetch_heif_deps.sh (macOS). Same upstream versions, same hashes,
# same decode-only flag set; the deltas are the toolchain (clang-cl + Ninja +
# static CRT), the install layout (DLLs in bin/, import libs in lib/) and the
# inspection tools (dumpbin/llvm-nm instead of nm/otool).
#
# LICENCE (do not "optimise" this away): libheif and libde265 are
# LGPL-3.0-or-later. They are built as SEPARATE SHARED LIBRARIES and linked
# dynamically, which satisfies LGPL-3 section 4(d)(1) outright -- a user can
# replace heif.dll / de265.dll next to the application. Static linking into
# dng_decoder_native would trigger the 4(d)(0) duty to ship relinkable object
# files with every release. WITH_X265 stays OFF because x265 is GPL-2.0.
#
# This script is intended to run on a windows-latest GitHub Actions runner
# under Git-Bash, driven by .github/workflows/heif_dist_windows.yml. Its output
# is COMMITTED to the repo (like the macOS dist), not rebuilt per CI run.
set -euo pipefail

HEIF_VERSION="1.23.2"
DE265_VERSION="1.1.1"
HEIF_URL="https://github.com/strukturag/libheif/releases/download/v${HEIF_VERSION}/libheif-${HEIF_VERSION}.tar.gz"
DE265_URL="https://github.com/strukturag/libde265/releases/download/v${DE265_VERSION}/libde265-${DE265_VERSION}.tar.gz"
HEIF_SHA256="8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405"
DE265_SHA256="fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST="${DNG_HEIF_DIST_OUT:-${NATIVE_DIR}/third_party/heif-dist-windows}"
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"
WANT_PINS="libheif=${HEIF_VERSION}:${HEIF_SHA256} libde265=${DE265_VERSION}:${DE265_SHA256} platform=windows-x86_64"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] \
   && [ -f "${DIST}/bin/heif.dll" ] && [ -f "${DIST}/bin/de265.dll" ]; then
  echo "[heif-win] dist already at the pinned versions:"
  echo "[heif-win]   libheif  ${HEIF_VERSION}  ${HEIF_SHA256}"
  echo "[heif-win]   libde265 ${DE265_VERSION}  ${DE265_SHA256}"
  exit 0
fi

# Downloads ${1} to ${2} and hard-fails unless its SHA-256 equals ${3}.
# A mismatch is never a warning: an unverified tarball must not reach a build
# whose output we then ship under an LGPL source-availability obligation.
fetch_verified() {
  local url="$1" dest="$2" want="$3" got=""
  if [ ! -f "${dest}" ]; then
    echo "[heif-win] downloading ${url}"
    curl -fsSL -o "${dest}.part" "${url}"
    mv "${dest}.part" "${dest}"
  fi
  got="$(sha256sum "${dest}" | awk '{print $1}')"
  if [ "${got}" != "${want}" ]; then
    echo "[heif-win] SHA-256 MISMATCH for ${dest}" >&2
    echo "[heif-win]   expected ${want}" >&2
    echo "[heif-win]   actual   ${got}" >&2
    rm -f "${dest}"
    exit 1
  fi
  echo "[heif-win] verified $(basename "${dest}") ${got}"
}

mkdir -p "${STAGE}"
fetch_verified "${HEIF_URL}"  "${STAGE}/libheif-${HEIF_VERSION}.tar.gz"   "${HEIF_SHA256}"
fetch_verified "${DE265_URL}" "${STAGE}/libde265-${DE265_VERSION}.tar.gz" "${DE265_SHA256}"

rm -rf "${STAGE}/libheif-${HEIF_VERSION}" "${STAGE}/libde265-${DE265_VERSION}"
tar -xzf "${STAGE}/libheif-${HEIF_VERSION}.tar.gz"   -C "${STAGE}"
tar -xzf "${STAGE}/libde265-${DE265_VERSION}.tar.gz" -C "${STAGE}"

COMMON_ARGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=clang-cl
  -DCMAKE_CXX_COMPILER=clang-cl
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DBUILD_SHARED_LIBS=ON
)

# --- libde265 first: libheif's FindLIBDE265 needs it installed on disk. ---
# ENABLE_ENCODER=OFF is the LGPL-cleanliness half of the decode-only rule;
# ENABLE_SDL/SHERLOCK265 off drops the GUI inspection tools and their deps.
#
# DEVIATION (build mechanism, not a feature reduction): libde265's
# CMakeLists.txt takes `if(MSVC)` to mean cl.exe, which enables the SSE4.1
# kernels while adding NO -m flag, because cl.exe accepts every intrinsic
# unconditionally. clang-cl sets MSVC=1 but is clang underneath and rejects
# `always_inline function '_mm_mullo_epi32' requires target feature 'sse4.1'`
# -- observed as 3 hard errors in run 33294113014. `/clang:-msse4.1` is the
# clang-cl spelling of the flag upstream would have passed on any non-MSVC
# compiler; it restores exactly the code paths upstream intended to build.
# Consequence, recorded in PROVENANCE.md: de265.dll requires an SSE4.1-capable
# CPU (Intel Penryn 2008+, AMD Bulldozer 2011+; Windows 11 already mandates
# SSE4.2). Turning ENABLE_SIMD off would have been the "green by building
# less" answer and is deliberately NOT taken.
#
# CMAKE_CXX_FLAGS on the command line REPLACES CMake's MSVC default
# ("/DWIN32 /D_WINDOWS /W3 /GR /EHsc"), so /EHsc and /GR are restored here by
# hand. Losing them silently disables C++ exceptions and RTTI for the whole
# libde265 build -- run 33294201597 surfaced that as "cannot use 'try' with
# exceptions disabled", but a library compiled without /EHsc that happens not
# to use `try` would have built green with broken unwinding semantics.
#
# ENABLE_DECODER=OFF gates ONLY the dec265 command-line TOOL subdirectory
# (libde265/CMakeLists.txt:241-243 builds the library unconditionally). The
# tool is not part of this dist, is never shipped, and its bundled getopt
# clone does not compile under clang's stricter C rules. The decode library
# itself is unaffected, which the heif_decode_image / de265-dependency
# assertions below still prove.
echo "[heif-win] configuring libde265 ${DE265_VERSION}"
cmake -S "${STAGE}/libde265-${DE265_VERSION}" -B "${STAGE}/build-de265" \
  "${COMMON_ARGS[@]}" \
  -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS /clang:-msse4.1" \
  -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc /GR /clang:-msse4.1" \
  -DENABLE_DECODER=OFF \
  -DENABLE_ENCODER=OFF \
  -DENABLE_SDL=OFF \
  -DENABLE_SHERLOCK265=OFF \
  -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF \
  -DWITH_FUZZERS=OFF
cmake --build "${STAGE}/build-de265" --parallel
cmake --install "${STAGE}/build-de265"

# The import library name differs between generators/toolchains (de265.lib vs
# libde265.lib). Resolve what was ACTUALLY installed and feed that to libheif's
# find module, instead of guessing and getting a silent LIBDE265_FOUND=false.
DE265_IMPLIB=""
for _cand in "${DIST}/lib/de265.lib" "${DIST}/lib/libde265.lib"; do
  if [ -f "${_cand}" ]; then
    DE265_IMPLIB="${_cand}"
    break
  fi
done
if [ -z "${DE265_IMPLIB}" ]; then
  echo "[heif-win] FAILED: no libde265 import library was installed." >&2
  find "${DIST}" -type f -name '*.lib' | sort >&2
  exit 1
fi
echo "[heif-win] libde265 import library: ${DE265_IMPLIB}"

# --- libheif, decode-only, against the just-installed libde265. ---
# WITH_LIBDE265=ON is the ONLY codec left on. X265 and AOM_ENCODER are named
# explicitly (x265 is GPL-2.0); the rest are off so a future upstream default
# flip cannot quietly pull an encoder in. WITH_LIBDE265_PLUGIN=OFF +
# ENABLE_PLUGIN_LOADING=OFF build the decoder INTO libheif: a dlopen-ed plugin
# directory would not survive application packaging.
# LIBDE265_INCLUDE_DIR/LIBDE265_LIBRARY are pre-seeded so the find module
# resolves to OUR dist and can never pick up something else on the runner.
echo "[heif-win] configuring libheif ${HEIF_VERSION}"
cmake -S "${STAGE}/libheif-${HEIF_VERSION}" -B "${STAGE}/build-heif" \
  "${COMMON_ARGS[@]}" \
  -DCMAKE_PREFIX_PATH="${DIST}" \
  -DLIBDE265_INCLUDE_DIR="${DIST}/include" \
  -DLIBDE265_LIBRARY="${DE265_IMPLIB}" \
  -DWITH_LIBDE265=ON \
  -DWITH_LIBDE265_PLUGIN=OFF \
  -DENABLE_PLUGIN_LOADING=OFF \
  -DWITH_X265=OFF \
  -DWITH_X264=OFF \
  -DWITH_KVAZAAR=OFF \
  -DWITH_UVG266=OFF \
  -DWITH_VVDEC=OFF \
  -DWITH_VVENC=OFF \
  -DWITH_AOM_DECODER=OFF \
  -DWITH_AOM_ENCODER=OFF \
  -DWITH_DAV1D=OFF \
  -DWITH_SvtEnc=OFF \
  -DWITH_RAV1E=OFF \
  -DWITH_OpenH264_DECODER=OFF \
  -DWITH_FFMPEG_DECODER=OFF \
  -DWITH_JPEG_DECODER=OFF \
  -DWITH_JPEG_ENCODER=OFF \
  -DWITH_OpenJPEG_DECODER=OFF \
  -DWITH_OpenJPEG_ENCODER=OFF \
  -DWITH_OPENJPH_ENCODER=OFF \
  -DWITH_UNCOMPRESSED_CODEC=OFF \
  -DWITH_HEADER_COMPRESSION=OFF \
  -DWITH_LIBSHARPYUV=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_GDK_PIXBUF=OFF \
  -DWITH_REDUCED_VISIBILITY=ON \
  -DBUILD_TESTING=OFF \
  -DWITH_FUZZERS=OFF
cmake --build "${STAGE}/build-heif" --parallel
cmake --install "${STAGE}/build-heif"

# Layout proof. CMake on Windows installs runtime DLLs under bin/ and import
# libraries under lib/. Asserting each file by name here means a layout change
# fails in this script -- where the message names the missing file -- rather
# than as an opaque find_library() failure inside the decoder's configure.
MISSING=""
for required in bin/heif.dll bin/de265.dll lib/heif.lib lib/de265.lib \
                include/libheif/heif.h include/libde265/de265.h; do
  if [ ! -f "${DIST}/${required}" ]; then
    MISSING="${MISSING} ${required}"
  fi
done
if [ -n "${MISSING}" ]; then
  echo "[heif-win] FAILED: dist is missing:${MISSING}" >&2
  echo "[heif-win] complete listing of what WAS installed:" >&2
  find "${DIST}" -type f -not -path "${STAGE}/*" | sort >&2
  exit 1
fi

# Proof, not assumption: a libheif built without a working libde265 configures
# and installs perfectly happily and then decodes nothing.
#
# Output is written to FILES and then grepped. Never pipe dumpbin into grep -q:
# under `set -o pipefail`, grep -q exits at its first match, the still-writing
# producer dies of SIGPIPE (141), and pipefail reports the pipeline as failed.
# The check would then fail precisely BECAUSE the symbol is present, and the
# contamination check (grep-found = bad) would never fire at all. Observed for
# real on 2026-08-28.
EXPORTS_TXT="${DIST}/.heif_exports.txt"
DEPS_TXT="${DIST}/.heif_deps.txt"

set +e
dumpbin -exports "${DIST}/bin/heif.dll" > "${EXPORTS_TXT}" 2>&1
RC=$?
set -e
echo "[heif-win] DUMPBIN_EXPORTS_RC=${RC}"
if [ "${RC}" -ne 0 ]; then
  echo "[heif-win] dumpbin unavailable (rc=${RC}); falling back to llvm-nm."
  set +e
  llvm-nm --extern-only --defined-only "${DIST}/bin/heif.dll" > "${EXPORTS_TXT}" 2>&1
  RC=$?
  set -e
  echo "[heif-win] LLVM_NM_RC=${RC}"
fi
if [ "${RC}" -ne 0 ]; then
  echo "[heif-win] FAILED: could not read heif.dll's export table with either tool." >&2
  cat "${EXPORTS_TXT}" >&2 || true
  exit 1
fi

set +e
dumpbin -dependents "${DIST}/bin/heif.dll" > "${DEPS_TXT}" 2>&1
RC=$?
set -e
echo "[heif-win] DUMPBIN_DEPENDENTS_RC=${RC}"
if [ "${RC}" -ne 0 ]; then
  set +e
  llvm-objdump -p "${DIST}/bin/heif.dll" > "${DEPS_TXT}" 2>&1
  RC=$?
  set -e
  echo "[heif-win] LLVM_OBJDUMP_RC=${RC}"
fi
if [ "${RC}" -ne 0 ]; then
  echo "[heif-win] FAILED: could not read heif.dll's import table with either tool." >&2
  exit 1
fi

set +e
grep -w "heif_decode_image" "${EXPORTS_TXT}" > /dev/null
RC=$?
set -e
echo "[heif-win] ASSERT heif_decode_image RC=${RC}"
if [ "${RC}" -ne 0 ]; then
  echo "[heif-win] FAILED: heif.dll exports no heif_decode_image" >&2
  exit 1
fi

set +e
grep -i "de265" "${DEPS_TXT}" > /dev/null
RC=$?
set -e
echo "[heif-win] ASSERT de265 dependency RC=${RC}"
if [ "${RC}" -ne 0 ]; then
  echo "[heif-win] FAILED: heif.dll has no de265 dependency -- it was built" >&2
  echo "[heif-win]   WITHOUT an HEVC decoder and would silently decode nothing." >&2
  cat "${DEPS_TXT}" >&2
  exit 1
fi

set +e
grep -i "x265" "${EXPORTS_TXT}" > /dev/null
RC=$?
set -e
echo "[heif-win] ASSERT no-x265 RC=${RC} (expected 1 = absent)"
if [ "${RC}" -eq 0 ]; then
  echo "[heif-win] FAILED: x265 symbols present (GPL-2.0 contamination)" >&2
  exit 1
fi

# Architecture proof, best-effort: `file` is present in Git-Bash. A dist
# silently built for the wrong architecture links nowhere, and the failure
# surfaces much later as an opaque linker message. If `file` is unavailable we
# print a NOTICE rather than pass silently.
set +e
command -v file > /dev/null 2>&1
RC=$?
set -e
if [ "${RC}" -eq 0 ]; then
  ARCH_TXT="${DIST}/.heif_arch.txt"
  file "${DIST}/bin/heif.dll" "${DIST}/bin/de265.dll" > "${ARCH_TXT}" 2>&1
  set +e
  grep -c "x86-64" "${ARCH_TXT}" > /dev/null
  RC=$?
  set -e
  echo "[heif-win] ASSERT PE32+ x86-64 RC=${RC}"
  if [ "${RC}" -ne 0 ]; then
    echo "[heif-win] FAILED: DLLs are not x86-64:" >&2
    cat "${ARCH_TXT}" >&2
    exit 1
  fi
  rm -f "${ARCH_TXT}"
else
  echo "[heif-win] NOTICE: 'file' unavailable; architecture check SKIPPED (not passed)."
fi

printf '%s' "${WANT_PINS}" > "${STAMP}"
rm -rf "${STAGE}"
echo "[heif-win] dist ready at ${DIST}"
echo "[heif-win]   libheif  ${HEIF_VERSION}"
echo "[heif-win]   libde265 ${DE265_VERSION}"
