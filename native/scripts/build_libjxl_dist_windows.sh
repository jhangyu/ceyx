#!/usr/bin/env bash
# Builds the WINDOWS libjxl (+ bundled highway and brotli) distribution:
# STATIC, into native/third_party/libjxl-dist-windows/.
#
# Sibling of fetch_libjxl_dist.sh (macOS/Linux). Same tag, same submodule pin
# mechanism, same JPEGXL_* flag set, same symbol assertions and licence
# vendoring -- the only deltas are the toolchain (clang-cl + Ninja + static
# CRT), the archive names (*.lib instead of lib*.a) and the inspection tool
# (llvm-nm instead of nm).
#
# DEVIATION FROM THE PLAN'S LITERAL Task-6 SNIPPET (documented, not a guess):
# the plan's snippet assumed a JXL_VERSION/JXL_SHA256 single-tarball pin,
# mirroring the single-tarball-pin mechanism fetch_heif_deps.sh (deleted
# round 5, D9) / fetch_libwebp_dist.sh (deleted round 3, D5) used to use --
# see native/third_party/heif-dist/PROVENANCE.md for the historical
# precedent. The macOS/Linux
# fetch_libjxl_dist.sh landed with a DIFFERENT mechanism instead: upstream
# libjxl has never published a release asset that is a source tarball
# including the third_party/highway and third_party/brotli git submodules the
# static build requires (see fetch_libjxl_dist.sh's own header for the full
# rationale, verified across every tag v0.6..v0.12.0). So there is no single
# URL+hash to pin against on either platform. This script therefore mirrors
# fetch_libjxl_dist.sh's ACTUAL mechanism -- git clone at a tagged commit,
# selective submodule init, pin on commit SHA + submodule SHAs -- rather than
# the plan's tarball-pin snippet. JXL_TAG below is grep-checked equal to
# fetch_libjxl_dist.sh's (Step 2), which is the load-bearing "one pin, two
# platforms" invariant the plan's literal JXL_SHA256 check would have
# enforced had the tarball mechanism existed.
#
# Runs on a windows-latest runner under Git-Bash, driven by
# .github/workflows/jxl_dist_windows.yml. Its output is COMMITTED, because no
# machine in this project can produce Windows binaries locally.
#
# KNOWN RISK, flagged rather than hidden: libjxl's build is the heaviest of
# the three Windows dists and its highway dependency has historically been
# the most compiler-sensitive part. If clang-cl cannot build it within the
# workflow timeout, the answer is NOT to disable features (no
# -DJPEGXL_ENABLE_SKCMS=OFF, no reduced highway target set) -- that produces a
# DLL that loads and then encodes nothing, which is strictly worse than an
# honest failure. Stop and report instead, exactly as the now-deleted
# build_heif_dist_windows.sh (round 5, D9) refused the same shortcut for
# libde265's SIMD kernels before it was retired in favour of the Python
# carrier (native/scripts/build_deps.py).
set -euo pipefail

JXL_TAG="v0.12.0"   # MUST equal fetch_libjxl_dist.sh's JXL_TAG. Grep-checked.
JXL_REPO_URL="https://github.com/libjxl/libjxl.git"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST="${CEYX_JXL_DIST_OUT:-${NATIVE_DIR}/third_party/libjxl-dist-windows}"
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"

# Windows archive names (clang-cl + MSVC-style .lib, not lib*.a). jxl_cms.lib
# is required in addition to the plan's literal list: the macOS/Linux dist
# hit the same gap (see native/third_party/libjxl-dist commit "fix(jxl): link
# libjxl_cms.a to resolve JxlGetDefaultCms") -- JxlGetDefaultCms lives in a
# separate cms archive, not in jxl.lib itself, on every platform.
REQUIRED_LIBS="jxl.lib jxl_threads.lib jxl_cms.lib hwy.lib brotlicommon.lib brotlidec.lib brotlienc.lib"

echo "[jxl-win] target tag ${JXL_TAG}"

mkdir -p "${STAGE}"
SRC="${STAGE}/libjxl"

# Only the submodules the static core library actually links need to be
# fetched -- same rationale as fetch_libjxl_dist.sh: brotli and highway
# (required unconditionally by encode/decode core) and skcms (required
# because this dist builds with -DJPEGXL_ENABLE_SKCMS=ON below).
JXL_NEEDED_SUBMODULES="third_party/brotli third_party/highway third_party/skcms"

if [ ! -d "${SRC}/.git" ]; then
  rm -rf "${SRC}"
  echo "[jxl-win] cloning ${JXL_REPO_URL} @ ${JXL_TAG} (submodules: ${JXL_NEEDED_SUBMODULES})"
  git clone --depth 1 --branch "${JXL_TAG}" "${JXL_REPO_URL}" "${SRC}"
  git -C "${SRC}" submodule update --init --depth 1 -- ${JXL_NEEDED_SUBMODULES}
fi

TOP_SHA="$(git -C "${SRC}" rev-parse HEAD)"
SUBMODULE_STATUS="$(git -C "${SRC}" submodule status -- ${JXL_NEEDED_SUBMODULES})"
WANT_PINS="tag=${JXL_TAG} commit=${TOP_SHA} platform=windows-x86_64
${SUBMODULE_STATUS}"

stamp_ok=1
[ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] || stamp_ok=0
for _l in ${REQUIRED_LIBS}; do
  [ -f "${DIST}/lib/${_l}" ] || stamp_ok=0
done
if [ "${stamp_ok}" -eq 1 ]; then
  echo "[jxl-win] dist already at the pinned commit: ${TOP_SHA}"
  exit 0
fi

echo "[jxl-win] pinned commit ${TOP_SHA}"
echo "[jxl-win] submodule pins:"
echo "${SUBMODULE_STATUS}"

rm -rf "${STAGE}/build"

# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded is mandatory for the same reason as
# every other Windows dist in this project: a /MD archive linked into the /MT
# decoder DLL fails as duplicate symbols or heap corruption, never as a clean
# configure error.
CMAKE_ARGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=clang-cl
  -DCMAKE_CXX_COMPILER=clang-cl
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_TESTING=OFF
  -DJPEGXL_ENABLE_TOOLS=OFF
  -DJPEGXL_ENABLE_BENCHMARK=OFF
  -DJPEGXL_ENABLE_EXAMPLES=OFF
  -DJPEGXL_ENABLE_FUZZERS=OFF
  -DJPEGXL_ENABLE_DOXYGEN=OFF
  -DJPEGXL_ENABLE_MANPAGES=OFF
  -DJPEGXL_ENABLE_SJPEG=OFF
  -DJPEGXL_ENABLE_OPENEXR=OFF
  -DJPEGXL_ENABLE_SKCMS=ON
  -DJPEGXL_ENABLE_JNI=OFF
  -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF
  -DJPEGXL_FORCE_SYSTEM_HWY=OFF
)

cmake -S "${SRC}" -B "${STAGE}/build" "${CMAKE_ARGS[@]}"
cmake --build "${STAGE}/build" --parallel
cmake --install "${STAGE}/build"

# FORCE_SYSTEM_* = OFF pins highway and brotli to the bundled submodule
# copies, so the submodule SHA set above covers the whole dependency closure.

MISSING=""
for _l in ${REQUIRED_LIBS}; do
  [ -f "${DIST}/lib/${_l}" ] || MISSING="${MISSING} ${_l}"
done
if [ -n "${MISSING}" ]; then
  echo "[jxl-win] FAILED: dist is missing:${MISSING}" >&2
  find "${DIST}" -type f -name '*.lib' | sort >&2
  exit 1
fi

# Symbol proof, capture-then-match. Never `llvm-nm | grep -q` under pipefail:
# grep exits at its first match, the still-writing producer dies of SIGPIPE,
# and pipefail reports the pipeline as failed precisely BECAUSE the symbol is
# present.
NM_OUT="${STAGE}/nm-jxl.txt"
llvm-nm --defined-only "${DIST}/lib/jxl.lib" > "${NM_OUT}" 2>&1 || true
for _sym in JxlEncoderProcessOutput JxlDecoderProcessInput JxlEncoderAddBox; do
  set +e
  grep -w "${_sym}" "${NM_OUT}" > /dev/null
  RC=$?
  set -e
  echo "[jxl-win] ASSERT ${_sym} RC=${RC}"
  if [ "${RC}" -ne 0 ]; then
    echo "[jxl-win] FAILED: ${_sym} not found in jxl.lib -- see the header comment" >&2
    echo "[jxl-win]   above: do NOT respond by disabling skcms or highway targets." >&2
    exit 1
  fi
done

for _pair in "libjxl:${SRC}" "highway:${SRC}/third_party/highway" "brotli:${SRC}/third_party/brotli"; do
  _name="${_pair%%:*}"; _dir="${_pair##*:}"
  mkdir -p "${DIST}/share/licenses/${_name}"
  find "${_dir}" -maxdepth 1 \( -iname 'LICENSE*' -o -iname 'COPYING*' \) \
       -exec cp {} "${DIST}/share/licenses/${_name}/" \;
  if [ -z "$(ls -A "${DIST}/share/licenses/${_name}")" ]; then
    echo "[jxl-win] FAILED: no licence file found for ${_name} under ${_dir}" >&2
    exit 1
  fi
done

printf '%s' "${WANT_PINS}" > "${STAMP}"
rm -rf "${STAGE}"
echo "[jxl-win] dist ready at ${DIST} (commit ${TOP_SHA})"
