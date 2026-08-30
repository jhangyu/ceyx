#!/usr/bin/env bash
# Vendors libheif + libde265 + kvazaar + aom as an ENCODE-ENABLED,
# DYNAMICALLY LINKED (libheif/libde265) distribution under
# native/third_party/heif-dist[-<arch>]/. Idempotent. Runs on macOS and Linux
# (ruling Q3).
#
# Usage:
#   fetch_heif_deps.sh                 # build everything, in order (default)
#   fetch_heif_deps.sh libde265        # build only libde265, then exit
#   fetch_heif_deps.sh kvazaar         # build only kvazaar, then exit
#   fetch_heif_deps.sh aom             # build only aom, then exit
#   fetch_heif_deps.sh libheif         # build only libheif (needs the three
#                                       # above already installed in DIST)
#   fetch_heif_deps.sh assemble        # run assertions + licence vendoring +
#                                       # stamp write + cleanup, no build
#
# The per-component split exists so each build is its own foreground
# invocation short enough to fit inside a normal command timeout, instead of
# one call that has to survive all four source builds back to back. Each
# component is independently idempotent (skips if its install artifact
# already exists); running with no argument chains all five stages exactly
# as this script always has.
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
# Everything the original spec DECIDED is preserved: dynamic linking for
# libheif/libde265, @rpath install names, placement next to the decoder dylib.
#
# kvazaar (HEVC encoder) and aom (AV1 encoder + decoder) are new as of the
# 2026-08-30 codec expansion. They are built STATIC and linked INTO libheif,
# because ENABLE_PLUGIN_LOADING=OFF -- a dlopen-ed plugin directory does not
# survive app-bundle packaging. This is licence-clean: both are BSD-family,
# and the LGPL object of concern (libheif) stays the dynamic, user-replaceable
# library.
set -euo pipefail

STAGE_ARG="${1:-all}"
case "${STAGE_ARG}" in
  all|libde265|kvazaar|aom|libheif|assemble) ;;
  *)
    echo "[heif] usage: $0 [all|libde265|kvazaar|aom|libheif|assemble]" >&2
    exit 2
    ;;
esac

HEIF_VERSION="1.23.2"
DE265_VERSION="1.1.1"
KVAZAAR_VERSION="2.3.1"
AOM_VERSION="3.12.1"
HEIF_URL="https://github.com/strukturag/libheif/releases/download/v${HEIF_VERSION}/libheif-${HEIF_VERSION}.tar.gz"
DE265_URL="https://github.com/strukturag/libde265/releases/download/v${DE265_VERSION}/libde265-${DE265_VERSION}.tar.gz"
KVAZAAR_URL="https://github.com/ultravideo/kvazaar/releases/download/v${KVAZAAR_VERSION}/kvazaar-${KVAZAAR_VERSION}.tar.gz"
AOM_URL="https://storage.googleapis.com/aom-releases/libaom-${AOM_VERSION}.tar.gz"
HEIF_SHA256="8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405"
DE265_SHA256="fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219"
# Verified against the release API / googlesource tag list on 2026-08-30 (see
# PROVENANCE.md); these are not assumptions, PROVENANCE.md:11-13 records a
# prior round that named versions that did not exist upstream.
KVAZAAR_SHA256="2510b8ecc2bf384bbc7b8fc2756bbfa8a8c173b57634c8dfdd8bea6733e56c46"
AOM_SHA256="9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e"

# Host OS detection -- six macOS-only assumptions upstream of this line become
# conditional per ruling Q3 (Linux gets HEIF/AVIF enabled, no committed dist).
# The macOS branch reproduces the historical behaviour exactly.
HOST_OS="$(uname -s)"
if [ "${HOST_OS}" = "Darwin" ]; then
  SHA_CMD="shasum -a 256"
  LIB_EXT="1.dylib"
  DE265_LIB_EXT="0.dylib"
  DE265_UNVERSIONED_EXT="dylib"
  NM_FLAGS="-gU"
  DEPS_CMD="otool -L"
  NPROC="$(sysctl -n hw.ncpu)"
else
  SHA_CMD="sha256sum"
  LIB_EXT="so.1"
  DE265_LIB_EXT="so.0"
  DE265_UNVERSIONED_EXT="so"
  NM_FLAGS="-D"
  DEPS_CMD="ldd"
  NPROC="$(nproc)"
fi

# Target architecture for the produced dylibs. Defaults to the host arch so an
# ordinary local build is unchanged; the macOS CI Intel leg cross-compiles the
# decoder for x86_64 on an Apple-silicon runner and must get an x86_64 dist too,
# otherwise the link fails on an architecture mismatch (the dist used to be
# hard-pinned to arm64). Override with DNG_HEIF_ARCH=x86_64.
HEIF_ARCH="${DNG_HEIF_ARCH:-$(uname -m)}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# The host-architecture dist keeps its historical path, so an ordinary local
# build is completely unaffected. A cross-architecture dist goes to a SUFFIXED
# directory instead of overwriting it: this tree is shared (other agents and the
# developer build arm64 from it concurrently), and silently replacing the arm64
# dylibs with x86_64 ones would break their next link with an error pointing at
# the decoder, not at this script.
HOST_ARCH="$(uname -m)"
if [ "${HEIF_ARCH}" = "${HOST_ARCH}" ]; then
  DIST="${NATIVE_DIR}/third_party/heif-dist"
else
  DIST="${NATIVE_DIR}/third_party/heif-dist-${HEIF_ARCH}"
fi
STAGE="${DIST}/.stage"
STAMP="${DIST}/.pins"
LIBHEIF_LIB="${DIST}/lib/libheif.${LIB_EXT}"
LIBDE265_LIB="${DIST}/lib/libde265.${DE265_LIB_EXT}"
KVAZAAR_LIB="${DIST}/lib/libkvazaar.a"
AOM_LIB="${DIST}/lib/libaom.a"
# The arch is part of the stamp: an arm64 dist and an x86_64 dist are not
# interchangeable, and without this a dist built for the other architecture
# would be reported as "already at the pinned versions" and then fail at link.
# kvazaar and aom versions/hashes are ALSO part of the stamp -- without this a
# pre-existing decode-only dist would match on libheif/libde265 alone, report
# "already at the pinned versions", exit 0, and the encoders would silently
# never appear. This is the highest-probability silent failure in this script.
WANT_PINS="libheif=${HEIF_VERSION}:${HEIF_SHA256} libde265=${DE265_VERSION}:${DE265_SHA256} kvazaar=${KVAZAAR_VERSION}:${KVAZAAR_SHA256} aom=${AOM_VERSION}:${AOM_SHA256} arch=${HEIF_ARCH}"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${WANT_PINS}" ] \
   && [ -f "${LIBHEIF_LIB}" ] && [ -f "${LIBDE265_LIB}" ]; then
  echo "[heif] dist already at the pinned versions:"
  echo "[heif]   libheif  ${HEIF_VERSION}  ${HEIF_SHA256}"
  echo "[heif]   libde265 ${DE265_VERSION}  ${DE265_SHA256}"
  echo "[heif]   kvazaar  ${KVAZAAR_VERSION}  ${KVAZAAR_SHA256}"
  echo "[heif]   aom      ${AOM_VERSION}  ${AOM_SHA256}"
  echo "[heif]   arch     ${HEIF_ARCH}"
  exit 0
fi

echo "[heif] building dist for architecture: ${HEIF_ARCH} (stage: ${STAGE_ARG})"
mkdir -p "${STAGE}"

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
  got="$(${SHA_CMD} "${dest}" | awk '{print $1}')"
  if [ "${got}" != "${want}" ]; then
    echo "[heif] SHA-256 MISMATCH for ${dest}" >&2
    echo "[heif]   expected ${want}" >&2
    echo "[heif]   actual   ${got}" >&2
    rm -f "${dest}"
    exit 1
  fi
  echo "[heif] verified $(basename "${dest}") ${got}"
}

COMMON_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DCMAKE_INSTALL_NAME_DIR=@rpath
  -DBUILD_SHARED_LIBS=ON
)
# STATIC_DEP_ARGS is COMMON_ARGS minus the shared-libs default and the rpath
# install name (both meaningless for a static archive): kvazaar and aom are
# built static and linked INTO libheif because ENABLE_PLUGIN_LOADING=OFF.
STATIC_DEP_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${DIST}"
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DBUILD_SHARED_LIBS=OFF
)
if [ "${HOST_OS}" = "Darwin" ]; then
  COMMON_ARGS+=(
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
    -DCMAKE_OSX_ARCHITECTURES="${HEIF_ARCH}"
  )
  STATIC_DEP_ARGS+=(
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
    -DCMAKE_OSX_ARCHITECTURES="${HEIF_ARCH}"
  )
fi
# -DCMAKE_POSITION_INDEPENDENT_CODE=ON is not optional: these static archives
# are linked into a shared library, and without PIC the link fails on Linux
# with a relocation error that names libheif rather than the archive.

# --- libde265 first: libheif's FindLIBDE265 needs it installed on disk. ---
# ENABLE_ENCODER=OFF is the LGPL-cleanliness half of the decode-only rule for
# libde265 itself; ENABLE_SDL/SHERLOCK265 off drops the GUI inspection tools
# and their deps.
build_libde265() {
  if [ -f "${LIBDE265_LIB}" ]; then
    echo "[heif] libde265 already installed at ${LIBDE265_LIB}, skipping"
    return 0
  fi
  fetch_verified "${DE265_URL}" "${STAGE}/libde265-${DE265_VERSION}.tar.gz" "${DE265_SHA256}"
  rm -rf "${STAGE}/libde265-${DE265_VERSION}"
  tar -xzf "${STAGE}/libde265-${DE265_VERSION}.tar.gz" -C "${STAGE}"
  echo "[heif] configuring libde265 ${DE265_VERSION}"
  cmake -S "${STAGE}/libde265-${DE265_VERSION}" -B "${STAGE}/build-de265" \
    "${COMMON_ARGS[@]}" \
    -DENABLE_DECODER=ON \
    -DENABLE_ENCODER=OFF \
    -DENABLE_SDL=OFF \
    -DENABLE_SHERLOCK265=OFF \
    -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF \
    -DWITH_FUZZERS=OFF
  cmake --build "${STAGE}/build-de265" --parallel "${NPROC}"
  cmake --install "${STAGE}/build-de265"
}

# --- kvazaar (HEVC encoder), static, linked into libheif. ---
build_kvazaar() {
  if [ -f "${KVAZAAR_LIB}" ]; then
    echo "[heif] kvazaar already installed at ${KVAZAAR_LIB}, skipping"
    return 0
  fi
  fetch_verified "${KVAZAAR_URL}" "${STAGE}/kvazaar-${KVAZAAR_VERSION}.tar.gz" "${KVAZAAR_SHA256}"
  rm -rf "${STAGE}/kvazaar-${KVAZAAR_VERSION}"
  tar -xzf "${STAGE}/kvazaar-${KVAZAAR_VERSION}.tar.gz" -C "${STAGE}"
  echo "[heif] configuring kvazaar ${KVAZAAR_VERSION} (static)"
  cmake -S "${STAGE}/kvazaar-${KVAZAAR_VERSION}" -B "${STAGE}/build-kvazaar" \
    "${STATIC_DEP_ARGS[@]}" \
    -DBUILD_TESTS=OFF
  cmake --build "${STAGE}/build-kvazaar" --parallel "${NPROC}"
  cmake --install "${STAGE}/build-kvazaar"
}

# --- aom (AV1 encoder + decoder), static, linked into libheif. ---
# CONFIG_AV1_ENCODER and CONFIG_AV1_DECODER are asserted independently later:
# they are independent flags and one can silently be off while the other is on.
build_aom() {
  if [ -f "${AOM_LIB}" ]; then
    echo "[heif] aom already installed at ${AOM_LIB}, skipping"
    return 0
  fi
  fetch_verified "${AOM_URL}" "${STAGE}/libaom-${AOM_VERSION}.tar.gz" "${AOM_SHA256}"
  rm -rf "${STAGE}/libaom-${AOM_VERSION}"
  tar -xzf "${STAGE}/libaom-${AOM_VERSION}.tar.gz" -C "${STAGE}"
  echo "[heif] configuring libaom ${AOM_VERSION} (static, encoder + decoder)"
  cmake -S "${STAGE}/libaom-${AOM_VERSION}" -B "${STAGE}/build-aom" \
    "${STATIC_DEP_ARGS[@]}" \
    -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_DOCS=OFF \
    -DENABLE_TOOLS=OFF \
    -DCONFIG_AV1_ENCODER=1 \
    -DCONFIG_AV1_DECODER=1
  cmake --build "${STAGE}/build-aom" --parallel "${NPROC}"
  cmake --install "${STAGE}/build-aom"
}

# --- libheif, against the already-installed libde265, kvazaar and aom. ---
# WITH_LIBDE265=ON, WITH_KVAZAAR=ON and WITH_AOM_DECODER/ENCODER=ON are the
# codecs enabled by the 2026-08-30 codec expansion. X265 and DAV1D/RAV1E stay
# OFF explicitly (x265 is GPL-2.0; ruling D2 excludes dav1d/rav1e) so a future
# upstream default flip cannot quietly pull them in.
# WITH_*_PLUGIN=OFF + ENABLE_PLUGIN_LOADING=OFF build every codec INTO
# libheif: a dlopen-ed plugin directory would not survive app-bundle packaging.
# *_INCLUDE_DIR/*_LIBRARY are pre-seeded so each find module resolves to OUR
# dist and can never pick up a Homebrew/system copy.
build_libheif() {
  for _dep in "${LIBDE265_LIB}" "${KVAZAAR_LIB}" "${AOM_LIB}"; do
    if [ ! -f "${_dep}" ]; then
      echo "[heif] FAILED: ${_dep} missing -- build libde265/kvazaar/aom first" >&2
      exit 1
    fi
  done
  fetch_verified "${HEIF_URL}" "${STAGE}/libheif-${HEIF_VERSION}.tar.gz" "${HEIF_SHA256}"
  rm -rf "${STAGE}/libheif-${HEIF_VERSION}"
  tar -xzf "${STAGE}/libheif-${HEIF_VERSION}.tar.gz" -C "${STAGE}"
  echo "[heif] configuring libheif ${HEIF_VERSION}"
  local _heif_extra_args=()
  if [ "${HOST_OS}" = "Darwin" ]; then
    # libheif's FindAOM.cmake tries find_package(AOM CONFIG) FIRST, before
    # falling back to the AOM_INCLUDE_DIR/AOM_LIBRARY hints below. On a dev
    # machine with `brew install aom`, CMake's default system prefix search
    # finds Homebrew's AOMConfig.cmake at /opt/homebrew/lib/cmake/AOM and
    # links against ITS shared libaom.dylib instead of our static archive --
    # our pre-seeded AOM_INCLUDE_DIR/AOM_LIBRARY hints are never even
    # consulted because the CONFIG branch already resolved AOM::aom. The
    # symptom is a green build with zero aom_codec_* symbols in libheif
    # (they live in the external Homebrew dylib, not merged in). Ignoring
    # Homebrew's prefix forces the CONFIG lookup to fail and fall through
    # to the hint-based path, same intent as pre-seeding LIBDE265_LIBRARY.
    _heif_extra_args+=("-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local")
  fi
  cmake -S "${STAGE}/libheif-${HEIF_VERSION}" -B "${STAGE}/build-heif" \
    "${COMMON_ARGS[@]}" \
    "${_heif_extra_args[@]}" \
    -DCMAKE_PREFIX_PATH="${DIST}" \
    -DLIBDE265_INCLUDE_DIR="${DIST}/include" \
    -DLIBDE265_LIBRARY="${DIST}/lib/libde265.${DE265_UNVERSIONED_EXT}" \
    -DWITH_LIBDE265=ON \
    -DWITH_LIBDE265_PLUGIN=OFF \
    -DENABLE_PLUGIN_LOADING=OFF \
    -DWITH_X265=OFF \
    -DWITH_X264=OFF \
    -DWITH_KVAZAAR=ON \
    -DWITH_KVAZAAR_PLUGIN=OFF \
    -DKVAZAAR_INCLUDE_DIR="${DIST}/include" \
    -DKVAZAAR_LIBRARY="${KVAZAAR_LIB}" \
    -DWITH_UVG266=OFF \
    -DWITH_VVDEC=OFF \
    -DWITH_VVENC=OFF \
    -DWITH_AOM_DECODER=ON \
    -DWITH_AOM_ENCODER=ON \
    -DWITH_AOM_DECODER_PLUGIN=OFF \
    -DWITH_AOM_ENCODER_PLUGIN=OFF \
    -DAOM_INCLUDE_DIR="${DIST}/include" \
    -DAOM_LIBRARY="${AOM_LIB}" \
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
  cmake --build "${STAGE}/build-heif" --parallel "${NPROC}"
  cmake --install "${STAGE}/build-heif"
}

# Runs every assertion, vendors licences, writes the .pins stamp and cleans
# up the stage directory. Split out so it can be its own short foreground
# invocation once all four builds above have already landed in DIST.
assemble() {
  for _dep in "${LIBHEIF_LIB}" "${LIBDE265_LIB}" "${KVAZAAR_LIB}" "${AOM_LIB}"; do
    if [ ! -f "${_dep}" ]; then
      echo "[heif] FAILED: ${_dep} missing -- run libde265/kvazaar/aom/libheif stages first" >&2
      exit 1
    fi
  done

  # Proof, not assumption: a libheif built without a working codec configures
  # and installs perfectly happily and then encodes/decodes nothing for that
  # codec.
  #
  # The symbol table and the load commands are captured into variables FIRST
  # and matched with here-strings, never as `nm ... | grep -q ...`. Under
  # this script's `set -o pipefail`, `grep -q` exits at its first match,
  # which kills the still-writing `nm` with SIGPIPE (exit 141); pipefail
  # then reports the pipeline as failed. The result is inverted: the check
  # fails precisely BECAUSE the symbol is present, and passes when it is
  # absent. Observed for real on 2026-08-28 -- a correct dist with
  # `_heif_decode_image` exported was rejected with "exports no
  # heif_decode_image". A here-string has no producer process, so there is
  # nothing to signal and the exit code means what it says.
  local heif_symbols heif_deps
  heif_symbols="$(nm ${NM_FLAGS} "${LIBHEIF_LIB}")"
  heif_deps="$(${DEPS_CMD} "${LIBHEIF_LIB}")"

  if ! grep -q 'heif_decode_image' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: libheif exports no heif_decode_image" >&2
    exit 1
  fi
  if ! grep -q 'libde265' <<< "${heif_deps}"; then
    echo "[heif] FAILED: libheif has no libde265 dependency -- it was built" >&2
    echo "[heif]   WITHOUT an HEVC decoder and would silently decode nothing." >&2
    exit 1
  fi
  if grep -q 'x265_encoder' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: x265 encoder symbols present (GPL-2.0 contamination)" >&2
    exit 1
  fi

  # --- Encoder assertions (2026-08-30 codec expansion) --------------------
  if ! grep -q 'heif_context_get_encoder_for_format' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: libheif exports no heif_context_get_encoder_for_format" >&2
    echo "[heif]   The dist was built decode-only; HEIC/AVIF encode cannot work." >&2
    exit 1
  fi
  if ! grep -q 'kvz_api_get' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: no kvazaar symbols in libheif -- WITH_KVAZAAR did not" >&2
    echo "[heif]   take effect. libheif configures and installs happily without" >&2
    echo "[heif]   an HEVC encoder and then encodes nothing." >&2
    exit 1
  fi
  # Both directions are asserted: WITH_AOM_ENCODER and WITH_AOM_DECODER are
  # INDEPENDENT flags, so one can silently be off while the other is on.
  if ! grep -q 'aom_codec_av1_cx' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: no aom ENCODER symbols in libheif (AVIF encode dead)" >&2
    exit 1
  fi
  if ! grep -q 'aom_codec_av1_dx' <<< "${heif_symbols}"; then
    echo "[heif] FAILED: no aom DECODER symbols in libheif (AVIF decode dead)" >&2
    exit 1
  fi

  if [ "${HOST_OS}" = "Darwin" ]; then
    # Arch proof, same "capture then match" discipline as the symbol checks
    # above: a dist silently built for the wrong architecture links nowhere,
    # and the failure surfaces much later as an opaque "building for
    # macOS-x86_64 but attempting to link file built for macOS-arm64".
    local _lib lib_archs
    for _lib in "${LIBHEIF_LIB}" "${LIBDE265_LIB}"; do
      lib_archs="$(lipo -archs "${_lib}")"
      if ! grep -q -w "${HEIF_ARCH}" <<< "${lib_archs}"; then
        echo "[heif] FAILED: $(basename "${_lib}") has archs '${lib_archs}', wanted '${HEIF_ARCH}'" >&2
        exit 1
      fi
    done
  else
    # Linux has no lipo/universal-binary concept; prove each output is a
    # real 64-bit ELF instead.
    local _lib file_out
    for _lib in "${LIBHEIF_LIB}" "${LIBDE265_LIB}"; do
      file_out="$(file "${_lib}")"
      if ! grep -q 'ELF 64-bit' <<< "${file_out}"; then
        echo "[heif] FAILED: ${_lib} is not a 64-bit ELF: ${file_out}" >&2
        exit 1
      fi
    done
  fi

  # --- Vendor licence files, BEFORE the stage cleanup below deletes the
  # --- source trees they live in.
  # PATENTS* is in the glob specifically for aom: it carries the Alliance
  # for Open Media Patent License 1.0 as a SEPARATE grant on top of BSD-2,
  # and shipping only LICENSE would drop it.
  local _srcdirs=("libheif-${HEIF_VERSION}" "libde265-${DE265_VERSION}" "kvazaar-${KVAZAAR_VERSION}" "libaom-${AOM_VERSION}")
  local _names=("libheif" "libde265" "kvazaar" "aom")
  local _urls=("${HEIF_URL}" "${DE265_URL}" "${KVAZAAR_URL}" "${AOM_URL}")
  local _shas=("${HEIF_SHA256}" "${DE265_SHA256}" "${KVAZAAR_SHA256}" "${AOM_SHA256}")
  local _i _src _name _tarball
  for _i in 0 1 2 3; do
    _src="${STAGE}/${_srcdirs[$_i]}"
    _name="${_names[$_i]}"
    # A resumed run (e.g. `libde265` stage run separately, already installed,
    # skipped straight to `return 0`) never extracts the source tree that the
    # build stage's own extraction step would have provided. assemble() runs
    # independently of which stages actually built anything, so re-fetch +
    # re-extract on demand here rather than assuming the tree is present.
    if [ ! -d "${_src}" ]; then
      _tarball="${STAGE}/${_srcdirs[$_i]}.tar.gz"
      fetch_verified "${_urls[$_i]}" "${_tarball}" "${_shas[$_i]}"
      tar -xzf "${_tarball}" -C "${STAGE}"
    fi
    mkdir -p "${DIST}/share/licenses/${_name}"
    find "${_src}" -maxdepth 1 \( -iname 'COPYING*' -o -iname 'LICENSE*' -o -iname 'PATENTS*' \) \
         -exec cp {} "${DIST}/share/licenses/${_name}/" \;
    if [ -z "$(ls -A "${DIST}/share/licenses/${_name}")" ]; then
      echo "[heif] FAILED: no licence file found for ${_name} in ${_src}" >&2
      exit 1
    fi
  done

  printf '%s' "${WANT_PINS}" > "${STAMP}"
  rm -rf "${STAGE}/build-de265" "${STAGE}/build-kvazaar" "${STAGE}/build-aom" "${STAGE}/build-heif" \
         "${STAGE}/libheif-${HEIF_VERSION}" "${STAGE}/libde265-${DE265_VERSION}" \
         "${STAGE}/kvazaar-${KVAZAAR_VERSION}" "${STAGE}/libaom-${AOM_VERSION}"
  echo "[heif] dist ready at ${DIST} (arch ${HEIF_ARCH})"
  echo "[heif]   libheif  ${HEIF_VERSION}"
  echo "[heif]   libde265 ${DE265_VERSION}"
  echo "[heif]   kvazaar  ${KVAZAAR_VERSION}"
  echo "[heif]   aom      ${AOM_VERSION}"
}

case "${STAGE_ARG}" in
  libde265) build_libde265 ;;
  kvazaar)  build_kvazaar ;;
  aom)      build_aom ;;
  libheif)  build_libheif ;;
  assemble) assemble ;;
  all)
    build_libde265
    build_kvazaar
    build_aom
    build_libheif
    assemble
    ;;
esac
