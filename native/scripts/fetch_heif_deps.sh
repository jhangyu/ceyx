#!/usr/bin/env bash
# Vendors libheif + libde265 as a DECODE-ONLY, DYNAMICALLY LINKED distribution
# under native/third_party/heif-dist/. Idempotent.
#
# Why a built dist instead of add_subdirectory (spec 7.3's wording):
#   1. libheif's cmake/modules/FindLIBDE265.cmake does find_library() for a
#      file on disk. An add_subdirectory()'d libde265 target does not exist as
#      a file at libheif's configure time, so LIBDE265_FOUND would be false and
#      libheif would build with NO HEVC decoder -- a green build that decodes
#      nothing.
#   2. BUILD_SHARED_LIBS is a global CACHE variable. Ceyx already vendors
#      LibRaw, RawSpeed3, zlib and libjpeg-turbo whose static-vs-shared choice
#      is deliberate (see third_party.cmake's App-Sandbox rationale for static
#      libjpeg). Flipping the global default under them is an unforced risk.
#   3. libheif sets CMAKE_CXX_STANDARD 20 and cmake_policy(VERSION 3.0...3.22)
#      at its top level; ceyx's project is C++17.
# Everything the spec DECIDED is preserved: dynamic linking, decode-only,
# no new package, @rpath install names, placement next to the decoder dylib.
set -euo pipefail

HEIF_VERSION="1.23.2"
DE265_VERSION="1.1.1"
HEIF_URL="https://github.com/strukturag/libheif/releases/download/v${HEIF_VERSION}/libheif-${HEIF_VERSION}.tar.gz"
DE265_URL="https://github.com/strukturag/libde265/releases/download/v${DE265_VERSION}/libde265-${DE265_VERSION}.tar.gz"
HEIF_SHA256="8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405"
DE265_SHA256="fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST="${NATIVE_DIR}/third_party/heif-dist"
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"
WANT_PINS="libheif=${HEIF_VERSION}:${HEIF_SHA256} libde265=${DE265_VERSION}:${DE265_SHA256}"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] \
   && [ -f "${DIST}/lib/libheif.1.dylib" ] && [ -f "${DIST}/lib/libde265.0.dylib" ]; then
  echo "[heif] dist already at the pinned versions:"
  echo "[heif]   libheif  ${HEIF_VERSION}  ${HEIF_SHA256}"
  echo "[heif]   libde265 ${DE265_VERSION}  ${DE265_SHA256}"
  exit 0
fi

# Downloads ${1} to ${2} and hard-fails unless its SHA-256 equals ${3}.
# A mismatch is never a warning: an unverified tarball must not reach a build
# whose output we then ship under an LGPL source-availability obligation.
fetch_verified() {
  local url="$1" dest="$2" want="$3" got=""
  if [ ! -f "${dest}" ]; then
    echo "[heif] downloading ${url}"
    curl -fsSL -o "${dest}.part" "${url}"
    mv "${dest}.part" "${dest}"
  fi
  got="$(shasum -a 256 "${dest}" | awk '{print $1}')"
  if [ "${got}" != "${want}" ]; then
    echo "[heif] SHA-256 MISMATCH for ${dest}" >&2
    echo "[heif]   expected ${want}" >&2
    echo "[heif]   actual   ${got}" >&2
    rm -f "${dest}"
    exit 1
  fi
  echo "[heif] verified $(basename "${dest}") ${got}"
}

mkdir -p "${STAGE}"
fetch_verified "${HEIF_URL}"  "${STAGE}/libheif-${HEIF_VERSION}.tar.gz"   "${HEIF_SHA256}"
fetch_verified "${DE265_URL}" "${STAGE}/libde265-${DE265_VERSION}.tar.gz" "${DE265_SHA256}"

rm -rf "${STAGE}/libheif-${HEIF_VERSION}" "${STAGE}/libde265-${DE265_VERSION}"
tar -xzf "${STAGE}/libheif-${HEIF_VERSION}.tar.gz"   -C "${STAGE}"
tar -xzf "${STAGE}/libde265-${DE265_VERSION}.tar.gz" -C "${STAGE}"

COMMON_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DCMAKE_INSTALL_NAME_DIR=@rpath
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  -DCMAKE_OSX_ARCHITECTURES=arm64
  -DBUILD_SHARED_LIBS=ON
)

# --- libde265 first: libheif's FindLIBDE265 needs it installed on disk. ---
# ENABLE_ENCODER=OFF is the LGPL-cleanliness half of the decode-only rule;
# ENABLE_SDL/SHERLOCK265 off drops the GUI inspection tools and their deps.
echo "[heif] configuring libde265 ${DE265_VERSION}"
cmake -S "${STAGE}/libde265-${DE265_VERSION}" -B "${STAGE}/build-de265" \
  "${COMMON_ARGS[@]}" \
  -DENABLE_DECODER=ON \
  -DENABLE_ENCODER=OFF \
  -DENABLE_SDL=OFF \
  -DENABLE_SHERLOCK265=OFF \
  -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF \
  -DWITH_FUZZERS=OFF
cmake --build "${STAGE}/build-de265" --parallel
cmake --install "${STAGE}/build-de265"

# --- libheif, decode-only, against the just-installed libde265. ---
# WITH_LIBDE265=ON is the ONLY codec left on. X265 and AOM_ENCODER are the two
# the spec names explicitly (x265 is GPL-2.0); the rest are turned off so a
# future upstream default flip cannot quietly pull an encoder in.
# WITH_LIBDE265_PLUGIN=OFF + ENABLE_PLUGIN_LOADING=OFF build the decoder INTO
# libheif: a dlopen-ed plugin directory would not survive app-bundle packaging.
# LIBDE265_INCLUDE_DIR/LIBDE265_LIBRARY are pre-seeded so the find module
# resolves to OUR dist and can never pick up a Homebrew libde265.
echo "[heif] configuring libheif ${HEIF_VERSION}"
cmake -S "${STAGE}/libheif-${HEIF_VERSION}" -B "${STAGE}/build-heif" \
  "${COMMON_ARGS[@]}" \
  -DCMAKE_PREFIX_PATH="${DIST}" \
  -DLIBDE265_INCLUDE_DIR="${DIST}/include" \
  -DLIBDE265_LIBRARY="${DIST}/lib/libde265.dylib" \
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

# Proof, not assumption: a libheif built without a working libde265 configures
# and installs perfectly happily and then decodes nothing.
#
# The symbol table and the load commands are captured into variables FIRST and
# matched with here-strings, never as `nm ... | grep -q ...`. Under this
# script's `set -o pipefail`, `grep -q` exits at its first match, which kills
# the still-writing `nm` with SIGPIPE (exit 141); pipefail then reports the
# pipeline as failed. The result is inverted: the check fails precisely BECAUSE
# the symbol is present, and passes when it is absent. Observed for real on
# 2026-08-28 -- a correct dist with `_heif_decode_image` exported was rejected
# with "exports no heif_decode_image". A here-string has no producer process,
# so there is nothing to signal and the exit code means what it says.
HEIF_SYMBOLS="$(nm -gU "${DIST}/lib/libheif.1.dylib")"
HEIF_DEPS="$(otool -L "${DIST}/lib/libheif.1.dylib")"

if ! grep -q 'heif_decode_image' <<< "${HEIF_SYMBOLS}"; then
  echo "[heif] FAILED: libheif exports no heif_decode_image" >&2
  exit 1
fi
if ! grep -q 'libde265' <<< "${HEIF_DEPS}"; then
  echo "[heif] FAILED: libheif has no libde265 dependency -- it was built" >&2
  echo "[heif]   WITHOUT an HEVC decoder and would silently decode nothing." >&2
  exit 1
fi
if grep -q 'x265_encoder' <<< "${HEIF_SYMBOLS}"; then
  echo "[heif] FAILED: x265 encoder symbols present (GPL-2.0 contamination)" >&2
  exit 1
fi

printf '%s' "${WANT_PINS}" > "${STAMP}"
rm -rf "${STAGE}/build-de265" "${STAGE}/build-heif" \
       "${STAGE}/libheif-${HEIF_VERSION}" "${STAGE}/libde265-${DE265_VERSION}"
echo "[heif] dist ready at ${DIST}"
echo "[heif]   libheif  ${HEIF_VERSION}"
echo "[heif]   libde265 ${DE265_VERSION}"
