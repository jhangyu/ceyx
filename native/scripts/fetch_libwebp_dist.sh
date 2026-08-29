#!/usr/bin/env bash
# Vendors libwebp as a STATIC, encode-capable distribution under
# native/third_party/libwebp-dist/. Idempotent.
#
# Why static (unlike the heif dist, which is deliberately dynamic):
# libwebp is BSD-3-Clause, so there is no LGPL relinking duty pushing us to a
# separate shared library, and third_party.cmake's 2026-08-17 App-Sandbox rule
# applies with full force — any absolute LC_LOAD_DYLIB into /opt/homebrew makes
# libdng_decoder_native.dylib unloadable inside a sandboxed host app. Static
# linking also keeps the shipped Libraries/ directory unchanged (no new dylib
# to codesign and stage).
#
# Why a built dist instead of add_subdirectory: libwebp's CMake sets
# BUILD_SHARED_LIBS-sensitive targets and its own install rules; ceyx already
# vendors zlib/libjpeg-turbo/LibRaw whose static-vs-shared choices are
# deliberate, and flipping global cache variables under them is an unforced
# risk (the same reasoning fetch_heif_deps.sh records).
set -euo pipefail

WEBP_VERSION="1.6.0"
WEBP_URL="https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WEBP_VERSION}.tar.gz"
WEBP_SHA256="e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"

# Target architecture. Defaults to the host arch; a cross-architecture dist
# goes to a SUFFIXED directory so it cannot clobber the arm64 dist a shared
# working tree is building against (same rule as fetch_heif_deps.sh).
WEBP_ARCH="${CEYX_WEBP_ARCH:-$(uname -m)}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
HOST_ARCH="$(uname -m)"
if [ "${WEBP_ARCH}" = "${HOST_ARCH}" ]; then
  DIST="${NATIVE_DIR}/third_party/libwebp-dist"
else
  DIST="${NATIVE_DIR}/third_party/libwebp-dist-${WEBP_ARCH}"
fi
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"
WANT_PINS="libwebp=${WEBP_VERSION}:${WEBP_SHA256} arch=${WEBP_ARCH}"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] \
   && [ -f "${DIST}/lib/libwebp.a" ] && [ -f "${DIST}/lib/libsharpyuv.a" ] \
   && [ -f "${DIST}/include/webp/encode.h" ]; then
  echo "[webp] dist already at the pinned version: ${WEBP_VERSION} (${WEBP_ARCH})"
  exit 0
fi

echo "[webp] building static dist ${WEBP_VERSION} for architecture: ${WEBP_ARCH}"

mkdir -p "${STAGE}"
TARBALL="${STAGE}/libwebp-${WEBP_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
  echo "[webp] downloading ${WEBP_URL}"
  curl -fsSL -o "${TARBALL}.part" "${WEBP_URL}"
  mv "${TARBALL}.part" "${TARBALL}"
fi
GOT="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${GOT}" != "${WEBP_SHA256}" ]; then
  echo "[webp] SHA-256 MISMATCH for ${TARBALL}" >&2
  echo "[webp]   expected ${WEBP_SHA256}" >&2
  echo "[webp]   actual   ${GOT}" >&2
  rm -f "${TARBALL}"
  exit 1
fi
echo "[webp] verified libwebp-${WEBP_VERSION}.tar.gz ${GOT}"

rm -rf "${STAGE}/libwebp-${WEBP_VERSION}" "${STAGE}/build"
tar -xzf "${TARBALL}" -C "${STAGE}"

# Encode + decode core only: no command-line tools, no mux/demux, no extras.
# The decoder half is kept because it costs nothing here and the harness uses
# WebPGetInfo to prove the produced bitstream is actually parseable.
cmake -S "${STAGE}/libwebp-${WEBP_VERSION}" -B "${STAGE}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${DIST}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCMAKE_OSX_ARCHITECTURES="${WEBP_ARCH}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DWEBP_BUILD_ANIM_UTILS=OFF \
  -DWEBP_BUILD_CWEBP=OFF \
  -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF \
  -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF \
  -DWEBP_BUILD_WEBPMUX=OFF \
  -DWEBP_BUILD_EXTRAS=OFF

cmake --build "${STAGE}/build" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
cmake --install "${STAGE}/build"

# Mechanical post-conditions: the encoder entry the FFI layer calls must exist
# in the archive, and the archive must be for the requested architecture.
# `nm | grep` is deliberately avoided: under `set -o pipefail` grep's early
# exit sends SIGPIPE to nm and inverts the gate.
NM_OUT="${STAGE}/nm-libwebp.txt"
nm -g "${DIST}/lib/libwebp.a" > "${NM_OUT}" 2>/dev/null || true
if ! grep -q "_WebPEncodeRGBA" "${NM_OUT}"; then
  echo "[webp] FATAL: WebPEncodeRGBA not found in ${DIST}/lib/libwebp.a" >&2
  exit 1
fi
HAVE_ARCHS="$(lipo -archs "${DIST}/lib/libwebp.a" 2>/dev/null || echo unknown)"
case " ${HAVE_ARCHS} " in
  *" ${WEBP_ARCH} "*) ;;
  *)
    echo "[webp] FATAL: libwebp.a has archs '${HAVE_ARCHS}', wanted '${WEBP_ARCH}'" >&2
    exit 1
    ;;
esac

echo "${WANT_PINS}" > "${STAMP}"
echo "[webp] dist ready: ${DIST} (archs: ${HAVE_ARCHS})"
