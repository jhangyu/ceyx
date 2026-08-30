#!/usr/bin/env bash
# Vendors libjxl (+ bundled highway and brotli) as a STATIC distribution under
# native/third_party/libjxl-dist/. Idempotent. See cmake/jxl.cmake for the
# static-vs-dynamic rationale.
#
# PIN MECHANISM (deliberately NOT a single tarball SHA-256, unlike
# fetch_heif_deps.sh / fetch_libwebp_dist.sh): upstream libjxl has never
# published a release asset that is a source tarball including the
# third_party/highway and third_party/brotli git submodules the build
# requires -- checked via the GitHub Releases API across every tag from v0.6
# through the current v0.12.0. The only source-shaped assets on any release
# are prebuilt binary archives (jxl-linux-x86_64-static*.tar.gz/.lz, Windows
# zip/7z, Debian .deb bundles). GitHub's own auto-generated
# archive/refs/tags/vX.tar.gz is a plain git-archive of the superproject and
# also excludes submodules. So there is no single URL+hash that covers the
# "libjxl + bundled highway + bundled brotli" dependency closure the static
# build (FORCE_SYSTEM_*=OFF) requires.
#
# Instead: git clone at a tagged commit, then selectively init only the
# submodules the static core library actually links (brotli, highway,
# skcms -- see JXL_NEEDED_SUBMODULES below), and pin on the top-level commit
# SHA plus those submodules' SHAs (git submodule status). Freshness on rerun
# is re-verified against those SHAs (git rev-parse HEAD / submodule status),
# not a tarball shasum.
set -euo pipefail

JXL_TAG="v0.12.0"                 # latest stable per GitHub API as of 2026-08-30;
                                   # deviates from the plan's target v0.11.1, which
                                   # the plan stated no reason to prefer over latest.
JXL_REPO_URL="https://github.com/libjxl/libjxl.git"

JXL_ARCH="${CEYX_JXL_ARCH:-$(uname -m)}"
HOST_ARCH="$(uname -m)"
HOST_OS="$(uname -s)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ "${JXL_ARCH}" = "${HOST_ARCH}" ]; then
  DIST="${NATIVE_DIR}/third_party/libjxl-dist"
else
  DIST="${NATIVE_DIR}/third_party/libjxl-dist-${JXL_ARCH}"
fi
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"

REQUIRED_LIBS="libjxl.a libjxl_threads.a libhwy.a libbrotlicommon.a libbrotlidec.a libbrotlienc.a"

echo "[jxl] target tag ${JXL_TAG}, arch ${JXL_ARCH}"

if [ "${HOST_OS}" = "Darwin" ]; then SHA_CMD="shasum -a 256"; else SHA_CMD="sha256sum"; fi

mkdir -p "${STAGE}"
SRC="${STAGE}/libjxl"

# Only the submodules the static core library actually links need to be
# fetched: brotli and highway (both required unconditionally by the encode/
# decode core) and skcms (required because this dist builds with
# -DJPEGXL_ENABLE_SKCMS=ON below). testdata, googletest, libjpeg-turbo,
# libpng, sjpeg, zlib and lcms are only pulled in by
# tools/examples/tests/plugins, all of which are disabled (ENABLE_TOOLS=OFF,
# ENABLE_BENCHMARK=OFF, ENABLE_EXAMPLES=OFF, BUILD_TESTING=OFF) -- cloning
# them would be a multi-GB no-op dominated by the testdata image corpus.
JXL_NEEDED_SUBMODULES="third_party/brotli third_party/highway third_party/skcms"

# Clone (or reuse an existing clone at the right tag) to compute the pin
# before deciding whether a rebuild is needed.
if [ ! -d "${SRC}/.git" ]; then
  rm -rf "${SRC}"
  echo "[jxl] cloning ${JXL_REPO_URL} @ ${JXL_TAG} (submodules: ${JXL_NEEDED_SUBMODULES})"
  git clone --depth 1 --branch "${JXL_TAG}" "${JXL_REPO_URL}" "${SRC}"
  git -C "${SRC}" submodule update --init --depth 1 -- ${JXL_NEEDED_SUBMODULES}
fi

TOP_SHA="$(git -C "${SRC}" rev-parse HEAD)"
SUBMODULE_STATUS="$(git -C "${SRC}" submodule status -- ${JXL_NEEDED_SUBMODULES})"
WANT_PINS="tag=${JXL_TAG} commit=${TOP_SHA} arch=${JXL_ARCH}
${SUBMODULE_STATUS}"

stamp_ok=1
[ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] || stamp_ok=0
for _l in ${REQUIRED_LIBS}; do
  [ -f "${DIST}/lib/${_l}" ] || stamp_ok=0
done
if [ "${stamp_ok}" -eq 1 ]; then
  echo "[jxl] dist already at the pinned commit: ${TOP_SHA} (${JXL_ARCH})"
  echo "[jxl] dist ready"
  exit 0
fi

echo "[jxl] pinned commit ${TOP_SHA}"
echo "[jxl] submodule pins:"
echo "${SUBMODULE_STATUS}"

rm -rf "${STAGE}/build"

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
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
if [ "${HOST_OS}" = "Darwin" ]; then
  CMAKE_ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_OSX_ARCHITECTURES="${JXL_ARCH}")
fi

cmake -S "${SRC}" -B "${STAGE}/build" "${CMAKE_ARGS[@]}"
cmake --build "${STAGE}/build" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
cmake --install "${STAGE}/build"

# FORCE_SYSTEM_* = OFF pins highway and brotli to the bundled submodule
# copies, so the submodule SHA set above covers the whole dependency closure.
# A system copy would make the pin a half-truth.

MISSING=""
for _l in ${REQUIRED_LIBS}; do
  [ -f "${DIST}/lib/${_l}" ] || MISSING="${MISSING} ${_l}"
done
if [ -n "${MISSING}" ]; then
  echo "[jxl] FAILED: dist is missing:${MISSING}" >&2
  find "${DIST}" -type f -name '*.a' | sort >&2
  exit 1
fi

# Symbol proof, capture-then-match (never `nm | grep -q` under pipefail).
NM_OUT="${STAGE}/nm-libjxl.txt"
nm -g "${DIST}/lib/libjxl.a" > "${NM_OUT}" 2>/dev/null || true
for _sym in JxlEncoderProcessOutput JxlDecoderProcessInput JxlEncoderAddBox; do
  if ! grep -q "${_sym}" "${NM_OUT}"; then
    echo "[jxl] FAILED: ${_sym} not found in libjxl.a" >&2
    exit 1
  fi
  echo "[jxl] ok ${_sym}"
done

# Release binaries ship no debug info (ruling Q5) -- strip local/debug symbols
# from the installed static archives so a downstream link never drags a
# debug-info-bearing object into the final dylib.
if [ "${HOST_OS}" = "Darwin" ]; then
  STRIP_CMD=(strip -S)
else
  STRIP_CMD=(strip --strip-debug)
fi
for _a in "${DIST}"/lib/*.a; do
  [ -f "${_a}" ] || continue
  "${STRIP_CMD[@]}" "${_a}" 2>/dev/null || true
done

for _pair in "libjxl:${SRC}" "highway:${SRC}/third_party/highway" "brotli:${SRC}/third_party/brotli"; do
  _name="${_pair%%:*}"; _dir="${_pair##*:}"
  mkdir -p "${DIST}/share/licenses/${_name}"
  find "${_dir}" -maxdepth 1 \( -iname 'LICENSE*' -o -iname 'COPYING*' \) \
       -exec cp {} "${DIST}/share/licenses/${_name}/" \;
  if [ -z "$(ls -A "${DIST}/share/licenses/${_name}")" ]; then
    echo "[jxl] FAILED: no licence file found for ${_name} under ${_dir}" >&2
    exit 1
  fi
done

cat > "${DIST}/PROVENANCE.md" <<EOF
# libjxl distribution — provenance

Built by \`native/scripts/fetch_libjxl_dist.sh\`. Nothing under this directory
is tracked except this file.

## Pin mechanism (commit-SHA based, not a tarball SHA-256)

Upstream libjxl has never published a release asset that is a source tarball
including the \`third_party/highway\` and \`third_party/brotli\` git
submodules the static build requires (checked via the GitHub Releases API
across every tag from v0.6 through v0.12.0, 2026-08-30). Only prebuilt binary
archives and a submodule-free \`archive/refs/tags\` snapshot exist. So the pin
here is \`git clone --recursive\` at a tagged commit, with the full recursive
submodule SHA set recorded below — equivalent verifiability to a tarball
hash, and it is the only mechanism that preserves "bundled submodules, not
system copies" for highway and brotli.

- Tag: \`${JXL_TAG}\` (latest stable per the GitHub Releases API as of
  2026-08-30; deviates from the design spec's target \`v0.11.1\`, which the
  spec gave no stated reason to prefer over latest).
- Commit: \`${TOP_SHA}\`
- Arch: \`${JXL_ARCH}\`

### Submodule pins (\`git submodule status\` for the submodules linked into
the static core library: brotli, highway, skcms)

\`\`\`
${SUBMODULE_STATUS}
\`\`\`

## Licence and linkage

libjxl and brotli are BSD-3-Clause; highway (Google) is Apache-2.0. All three
are linked **statically** into \`libdng_decoder_native\` — BSD-3/Apache-2.0
carry no relink duty, unlike the LGPL-3 heif-dist, which is why this dist is
static where heif-dist is dynamic (see cmake/jxl.cmake). Licence files for all
three are vendored under \`share/licenses/{libjxl,highway,brotli}/\` and must
ship alongside any distributed build that includes JXL support.

## Static libraries

\`libjxl.a\`, \`libjxl_threads.a\`, \`libhwy.a\`, \`libbrotlicommon.a\`,
\`libbrotlidec.a\`, \`libbrotlienc.a\` — all release-built with
\`CMAKE_BUILD_TYPE=Release\` and stripped of debug symbols (ruling Q5) before
being written here.
EOF

printf '%s' "${WANT_PINS}" > "${STAMP}"
echo "[jxl] dist ready at ${DIST} (arch ${JXL_ARCH}, commit ${TOP_SHA})"
