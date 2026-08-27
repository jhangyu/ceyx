#!/usr/bin/env bash
# Regenerates the H1 known-answer fixtures on a macOS host. Run once; the
# outputs are committed so the gate needs no phone and no network.
#
# The reference MUST come from an implementation that is not ours. macOS
# ImageIO (via sips) is that implementation: it encodes the sample HEIC from a
# plain JPEG and independently decodes it back to PNG. Comparing libheif's
# decode against ImageIO's decode of the same coded bitstream is what makes
# this a known-answer test rather than a tautology.
#
# Keep the fixture small (long edge <= 512) so it can live in git.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_JPEG="${1:-}"
if [ -z "${SOURCE_JPEG}" ] || [ ! -f "${SOURCE_JPEG}" ]; then
  echo "usage: make_h1_fixtures.sh <source.jpg>" >&2
  echo "  pick a photographic JPEG with real colour content -- a synthetic" >&2
  echo "  flat-colour image would pass even with a broken YUV matrix." >&2
  exit 2
fi

WORK="${SCRIPT_DIR}/.h1work"
rm -rf "${WORK}"
mkdir -p "${WORK}"

# 1. Downscale to a committable size, staying photographic.
sips -Z 512 "${SOURCE_JPEG}" --out "${WORK}/small.jpg" > /dev/null

# 2. Encode HEIC with ImageIO.
sips -s format heic "${WORK}/small.jpg" --out "${SCRIPT_DIR}/h1_sample.heic" > /dev/null

# 3. Decode that same HEIC back to PNG with ImageIO -- the independent answer.
sips -s format png "${SCRIPT_DIR}/h1_sample.heic" --out "${WORK}/h1_reference.png" > /dev/null

# 4. Convert to the sidecar the C gate reads.
python3 "${SCRIPT_DIR}/png_to_rgba.py" "${WORK}/h1_reference.png" \
        "${SCRIPT_DIR}/h1_reference.rgba"

rm -rf "${WORK}"
echo "[h1] fixtures ready:"
ls -l "${SCRIPT_DIR}/h1_sample.heic" "${SCRIPT_DIR}/h1_reference.rgba"
shasum -a 256 "${SCRIPT_DIR}/h1_sample.heic" "${SCRIPT_DIR}/h1_reference.rgba"
