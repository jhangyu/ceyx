#!/usr/bin/env bash
# Builds the WINDOWS libwebp distribution: STATIC, encode + decode + mux, into
# native/third_party/libwebp-dist-windows/.
#
# WEBP_VERSION below MUST equal native/vcpkg/vcpkg.json's libwebp override
# ("version": "1.6.0") -- that manifest is what macOS/Linux resolve libwebp
# from since D5 (round 3; the prior fetch_libwebp_dist.sh shell script this
# comment used to reference was deleted in that change). A Windows dist at a
# different version than the vcpkg-pinned one is a silent behavioural fork
# nothing would catch.
#
# Runs on a windows-latest runner under Git-Bash, driven by
# .github/workflows/webp_dist_windows.yml. Its output is COMMITTED, like the
# heif Windows dist, because no machine in this project can produce Windows
# binaries locally (cmake/heif.cmake:28-35).
#
# STATIC, unlike the heif dist: libwebp is BSD-3-Clause with no LGPL relink
# duty (cmake/encode.cmake), so there is no new DLL to stage or sign.
set -euo pipefail

WEBP_VERSION="1.6.0"
WEBP_URL="https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WEBP_VERSION}.tar.gz"
WEBP_SHA256="e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST="${CEYX_WEBP_DIST_OUT:-${NATIVE_DIR}/third_party/libwebp-dist-windows}"
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"
WANT_PINS="libwebp=${WEBP_VERSION}:${WEBP_SHA256} platform=windows-x86_64 archives=webp+mux+demux+sharpyuv"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] \
   && [ -f "${DIST}/lib/libwebp.lib" ] && [ -f "${DIST}/lib/libwebpmux.lib" ]; then
  echo "[webp-win] dist already at the pinned version ${WEBP_VERSION}"
  exit 0
fi

mkdir -p "${STAGE}"
TARBALL="${STAGE}/libwebp-${WEBP_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
  echo "[webp-win] downloading ${WEBP_URL}"
  curl -fsSL -o "${TARBALL}.part" "${WEBP_URL}"
  mv "${TARBALL}.part" "${TARBALL}"
fi
GOT="$(sha256sum "${TARBALL}" | awk '{print $1}')"
if [ "${GOT}" != "${WEBP_SHA256}" ]; then
  echo "[webp-win] SHA-256 MISMATCH: expected ${WEBP_SHA256}, got ${GOT}" >&2
  rm -f "${TARBALL}"
  exit 1
fi
echo "[webp-win] verified libwebp-${WEBP_VERSION}.tar.gz ${GOT}"

rm -rf "${STAGE}/libwebp-${WEBP_VERSION}" "${STAGE}/build"
tar -xzf "${TARBALL}" -C "${STAGE}"

# MultiThreaded (/MT) matches the decoder DLL's CRT, selected by
# native/CMakeLists.txt's CMP0091 opt-in. A /MD archive linked into a /MT
# DLL surfaces as duplicate symbols or heap corruption, never as a clean error.
#
# WEBP_BUILD_* flags disable the command-line TOOLS only. The mux and demux
# LIBRARIES are built regardless -- that distinction is the whole reason WebP
# metadata support needs no new dependency.
cmake -S "${STAGE}/libwebp-${WEBP_VERSION}" -B "${STAGE}/build" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-cl \
  -DCMAKE_CXX_COMPILER=clang-cl \
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
  -DCMAKE_INSTALL_PREFIX="${DIST}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
cmake --build "${STAGE}/build" --parallel
cmake --install "${STAGE}/build"

# Layout proof: assert each required file by name here, where the message names
# the missing file, rather than as an opaque find_library() failure inside the
# decoder's configure much later.
# NAME NOTE (observed for real in run 33307093239, not a guess): CMake's
# install step keeps the "lib" prefix on the archive names even under the
# clang-cl + Ninja toolchain (CMAKE_STATIC_LIBRARY_PREFIX is derived from the
# compiler ID -- Clang, not MSVC's cl.exe -- so it defaults to "lib" here,
# unlike a pure MSVC build which would install webp.lib). Consumers
# (cmake/encode.cmake's find_library(NAMES webp)) already search both the
# prefixed and unprefixed spellings, so only THIS script's own hardcoded
# assertions needed correcting.
MISSING=""
for required in lib/libwebp.lib lib/libwebpmux.lib lib/libwebpdemux.lib lib/libsharpyuv.lib \
                include/webp/encode.h include/webp/decode.h include/webp/mux.h; do
  [ -f "${DIST}/${required}" ] || MISSING="${MISSING} ${required}"
done
if [ -n "${MISSING}" ]; then
  echo "[webp-win] FAILED: dist is missing:${MISSING}" >&2
  find "${DIST}" -type f -not -path "${STAGE}/*" | sort >&2
  exit 1
fi

# Symbol proof. Output to FILES, then grep the files -- never
# `llvm-nm | grep -q`, which under pipefail kills the producer with SIGPIPE on
# a SUCCESSFUL match and reports 141.
ENC_TXT="${DIST}/.webp_enc_syms.txt"
MUX_TXT="${DIST}/.webp_mux_syms.txt"
llvm-nm --defined-only "${DIST}/lib/libwebp.lib"    > "${ENC_TXT}" 2>&1 || true
llvm-nm --defined-only "${DIST}/lib/libwebpmux.lib" > "${MUX_TXT}" 2>&1 || true

assert_sym() {  # $1 = symbol, $2 = file, $3 = human label
  set +e
  grep -w "$1" "$2" > /dev/null
  local rc=$?
  set -e
  echo "[webp-win] ASSERT $3 $1 RC=${rc}"
  if [ "${rc}" -ne 0 ]; then
    echo "[webp-win] FAILED: $1 absent from $2" >&2
    exit 1
  fi
}
assert_sym WebPEncodeRGBA          "${ENC_TXT}" "encoder"
assert_sym WebPEncodeLosslessRGBA  "${ENC_TXT}" "lossless encoder"
assert_sym WebPMuxSetChunk         "${MUX_TXT}" "mux"
assert_sym WebPMuxAssemble         "${MUX_TXT}" "mux"

mkdir -p "${DIST}/share/licenses/libwebp"
cp "${STAGE}/libwebp-${WEBP_VERSION}/COPYING" "${DIST}/share/licenses/libwebp/"

printf '%s' "${WANT_PINS}" > "${STAMP}"
rm -rf "${STAGE}"
echo "[webp-win] dist ready at ${DIST}"
