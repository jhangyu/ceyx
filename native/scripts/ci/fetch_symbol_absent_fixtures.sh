#!/usr/bin/env bash
# Fetches the two pinned, older macOS dylib releases that the plugin's
# symbol-absent contract tests need as fixtures:
#   - v0.1.15 macos-arm64 -> plugin/../tmp/old-dylib-wp10/
#     (predates the R4 WP10 probe/decode-into-buffer symbol pair; see
#     plugin/test/wp10_decode_into_buffer_symbol_absent_test.dart)
#   - v0.1.14 macos-arm64 -> plugin/../tmp/old-dylib-slotcfg/
#     (predates the R4 item 1 slot-configuration symbol group added in
#     commit 543bf074387b2c0df5adb079af3f29654a17c5ab; see
#     plugin/test/slot_config_symbol_absent_test.dart)
#
# Fetched BY TAG (no GitHub API listing call), same anonymous-rate-limit-safe
# pattern Halcyon's build_apps.py uses for its ceyx release pin. Digests below
# were recomputed locally against both the release asset and the pre-existing
# gitignored tmp/ copies before being pinned here — never transcribe a digest
# into this file without recomputing it yourself.
#
# Fails LOUDLY (set -e, explicit sha256 compare) on any mismatch or fetch
# failure. Run from the repository root:
#   bash native/scripts/ci/fetch_symbol_absent_fixtures.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

fetch_and_verify() {
  local tag="$1" dest_dir="$2" expected_sha="$3"
  local url="https://github.com/jhangyu/ceyx/releases/download/${tag}/dng_decoder_native-macos-arm64.tar.gz"
  local tmp_tar
  tmp_tar="$(mktemp)"

  echo "::group::Fetch ${tag} macos-arm64 fixture -> ${dest_dir}"
  curl -fsSL -o "${tmp_tar}" "${url}"

  rm -rf "${dest_dir}"
  mkdir -p "${dest_dir}"
  tar -xzf "${tmp_tar}" -C "${dest_dir}"
  rm -f "${tmp_tar}"

  local dylib="${dest_dir}/libdng_decoder_native.dylib"
  if [ ! -f "${dylib}" ]; then
    echo "::error::${dylib} missing after extracting ${tag} asset" >&2
    exit 1
  fi

  local actual_sha
  actual_sha="$(shasum -a 256 "${dylib}" | awk '{print $1}')"
  if [ "${actual_sha}" != "${expected_sha}" ]; then
    echo "::error::sha256 mismatch for ${dylib}: expected ${expected_sha}, got ${actual_sha}" >&2
    exit 1
  fi
  echo "sha256 verified: ${dylib} == ${expected_sha}"
  echo "::endgroup::"
}

# v0.1.15 macos-arm64 libdng_decoder_native.dylib — lacks the WP10
# probe/decode-into-buffer pair (dng_probe_output_size / dng_decode_into_buffer).
fetch_and_verify "v0.1.15" "${REPO_ROOT}/tmp/old-dylib-wp10" \
  "4e6ae55f5472a6c0a7e1cf806c063284dbeb6f0018ad5f44b926400a6af8a10b"

# v0.1.14 macos-arm64 libdng_decoder_native.dylib — predates commit 543bf07
# (dng_decode_configure_slots and its three siblings), so it lacks the whole
# slot-configuration symbol group.
fetch_and_verify "v0.1.14" "${REPO_ROOT}/tmp/old-dylib-slotcfg" \
  "6a995748ed55fca1d7c0ea27bc598bbce34afd66157d1fea5968d43359ff812e"

echo "Both symbol-absent fixtures fetched and verified."
