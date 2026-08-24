#!/usr/bin/env bash
# Vendors LibRaw (with its bundled RawSpeed3) at the revisions pinned by
# docs/raw_pipeline_contract_spec.md section 14. Idempotent.
#
# Each component is fetched via a throwaway git checkout, then its .git
# directory is stripped and the resolved revision is recorded in a
# .vendor-rev sidecar file instead. This is required because a nested .git
# directory makes the parent repo treat the whole vendored tree as an opaque
# untracked boundary (like a submodule gitlink), which would make it
# impossible to track PROVENANCE.md inside third_party/libraw/ per this
# project's vendoring policy (untracked source, tracked PROVENANCE.md).
set -euo pipefail

LIBRAW_REV="df226ea4178ccd74245f4f13c23adddfa01411c9"
RAWSPEED_REV="de70ef5fbc62cde91009c8cff7a206272abe631e"
# LibRaw ships no CMakeLists.txt of its own at the pinned revision (see
# third_party/libraw/README.cmake). This project vendors the community
# LibRaw/LibRaw-cmake overlay as a third pinned dependency; see PROVENANCE.md.
LIBRAW_CMAKE_REV="eb98e4325aef2ce85d2eb031c2ff18640ca616d3"
LIBRAW_URL="https://github.com/LibRaw/LibRaw.git"
RAWSPEED_URL="https://github.com/darktable-org/rawspeed.git"
LIBRAW_CMAKE_URL="https://github.com/LibRaw/LibRaw-cmake.git"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST="${NATIVE_DIR}/third_party/libraw"
RS_DEST="${DEST}/RawSpeed3/rawspeed"
LIBRAW_CMAKE_DEST="${NATIVE_DIR}/third_party/libraw-cmake"

# Fetches ${url} at ${rev} into ${dir} via a throwaway git checkout, unless
# ${dir}/.vendor-rev already records ${rev}. Leaves ${dir}/.git in place so
# callers needing git operations (e.g. `git apply` for patches) can use it;
# strip_git() below removes it once such operations are done.
clone_at() {
  local url="$1" rev="$2" dir="$3" backup=""
  if [ -f "${dir}/.vendor-rev" ] && [ "$(cat "${dir}/.vendor-rev")" = "${rev}" ]; then
    echo "[fetch] ${dir} already at ${rev} (.vendor-rev)"
    return 0
  fi
  # PROVENANCE.md (if this is third_party/libraw/) is a tracked file that
  # lives alongside the vendored source; re-fetching must not destroy it.
  if [ -f "${dir}/PROVENANCE.md" ]; then
    backup="$(mktemp)"
    cp "${dir}/PROVENANCE.md" "${backup}"
  fi
  rm -rf "${dir}"
  mkdir -p "${dir}"
  git -C "${dir}" init -q
  git -C "${dir}" remote add origin "${url}"
  git -C "${dir}" fetch -q --depth 1 origin "${rev}"
  git -C "${dir}" checkout -q "${rev}"
  if [ -n "${backup}" ]; then
    mv "${backup}" "${dir}/PROVENANCE.md"
  fi
}

# Removes ${dir}/.git and records the resolved HEAD into ${dir}/.vendor-rev,
# so the parent repo can track ordinary files inside ${dir} (e.g.
# PROVENANCE.md) instead of treating it as an opaque nested-repo boundary.
strip_git() {
  local dir="$1" rev
  rev="$(git -C "${dir}" rev-parse HEAD)"
  rm -rf "${dir}/.git"
  printf '%s\n' "${rev}" > "${dir}/.vendor-rev"
}

clone_at "${LIBRAW_URL}"       "${LIBRAW_REV}"       "${DEST}"
clone_at "${RAWSPEED_URL}"     "${RAWSPEED_REV}"     "${RS_DEST}"
clone_at "${LIBRAW_CMAKE_URL}" "${LIBRAW_CMAKE_REV}" "${LIBRAW_CMAKE_DEST}"

PATCH_DIR="${DEST}/RawSpeed3/patches"
if [ -d "${PATCH_DIR}" ] && [ -d "${RS_DEST}/.git" ]; then
  for p in "${PATCH_DIR}"/*.patch; do
    [ -e "${p}" ] || continue
    if git -C "${RS_DEST}" apply --check "${p}" 2>/dev/null; then
      git -C "${RS_DEST}" apply "${p}"; echo "[fetch] applied $(basename "${p}")"
    elif git -C "${RS_DEST}" apply --check --reverse "${p}" 2>/dev/null; then
      # At this RawSpeed revision the tree already matches the patch's
      # post-state (e.g. class already has `final`), but LibRaw's patch
      # exists to REMOVE that qualifier for subclass extensibility (see
      # third_party/libraw/PROVENANCE.md "Revision pin substitutions").
      # A reverse-check success here means we must apply --reverse to reach
      # the actually-required pre-state, not skip it as a no-op.
      git -C "${RS_DEST}" apply --reverse "${p}"
      echo "[fetch] applied (reverse) $(basename "${p}")"
    else
      echo "[fetch] FAILED to apply $(basename "${p}") at RawSpeed ${RAWSPEED_REV}" >&2
      echo "[fetch] Pin a RawSpeed revision where the patch set applies cleanly," >&2
      echo "[fetch] and record the substitution in third_party/libraw/PROVENANCE.md." >&2
      exit 1
    fi
  done
fi

for d in "${DEST}" "${RS_DEST}" "${LIBRAW_CMAKE_DEST}"; do
  if [ -d "${d}/.git" ]; then
    strip_git "${d}"
  fi
done

echo "[fetch] LibRaw ${LIBRAW_REV} + RawSpeed ${RAWSPEED_REV} + LibRaw-cmake ${LIBRAW_CMAKE_REV} ready at ${DEST}"
