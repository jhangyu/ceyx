#!/usr/bin/env bash
# Vendors LibRaw (with its bundled RawSpeed3) at the revisions pinned by
# docs/spec/raw_pipeline_contract_spec.md section 14. Idempotent.
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
RAWSPEED_REV="c835b05aecfacb7343f7c424abd620aa12116c3f"
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
PROJECT_PATCH_DIR="${NATIVE_DIR}/patches/libraw"

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
# LibRaw ships its RawSpeed3 patch set INSIDE its own source tree, so
# ${PATCH_DIR} is wiped and restored to LibRaw's originals by every LibRaw
# re-clone above. LibRaw states its patches are valid only for one specific
# RawSpeed commit-id (RawSpeed3/README.md), and Phase 19 re-pinned RawSpeed3
# five years forward, so the set had to be re-ported. Keeping the re-ports as
# in-place edits of ${PATCH_DIR} would make them unreproducible (destroyed by
# the next fetch) -- exactly the "vendored edit recorded nowhere" this project
# forbids. Instead the re-ported set is a tracked project directory and is
# overlaid here, REPLACING LibRaw's originals wholesale: the replacement is
# total (not a merge) so that a patch dropped as upstream-fixed stays dropped
# rather than reappearing from LibRaw's tree. See third_party/libraw/
# PROVENANCE.md "RawSpeed3 re-pin (Phase 19 W1)".
RAWSPEED_PATCH_SRC="${NATIVE_DIR}/patches/rawspeed3"
if [ -d "${RAWSPEED_PATCH_SRC}" ]; then
  rm -rf "${PATCH_DIR}"
  mkdir -p "${PATCH_DIR}"
  cp "${RAWSPEED_PATCH_SRC}"/*.patch "${PATCH_DIR}/"
  echo "[fetch] overlaid re-ported RawSpeed3 patch set from ${RAWSPEED_PATCH_SRC}"
fi

if [ -d "${PATCH_DIR}" ] && [ -d "${RS_DEST}/.git" ]; then
  for p in "${PATCH_DIR}"/*.patch; do
    [ -e "${p}" ] || continue
    if git -C "${RS_DEST}" apply --check "${p}" 2>/dev/null; then
      git -C "${RS_DEST}" apply "${p}"; echo "[fetch] applied $(basename "${p}")"
    elif git -C "${RS_DEST}" apply --check --reverse "${p}" 2>/dev/null; then
      # The tree already matches this patch's post-state, so there is nothing
      # to do: SKIP, exactly as the project-patch loop below does.
      #
      # This branch used to run `git apply --reverse` instead. That was correct
      # only for the pre-Phase-19 arrangement, where LibRaw's own patch 01 was
      # stored INVERTED relative to its intent (its diff added `final`, while
      # its purpose was to remove it), so reaching the desired state genuinely
      # meant reverse-applying. Since the Phase 19 re-pin, every patch in
      # ${PATCH_DIR} is overlaid from patches/rawspeed3/ and is forward-authored
      # against the pinned revision: applying it FORWARD is what reaches the
      # desired state, and REVERSE_APPLIED_PATCHES in verify_raw_provenance.py
      # is empty to match.
      #
      # For a forward-authored patch, reverse-check success means "already
      # applied", and reverse-applying it would UNDO our own change while
      # printing a success line and exiting 0. The realistic trigger is a future
      # re-pin in which upstream adopts one of our changes (upstream already did
      # exactly this once, with patch 01's `final` removal): the forward check
      # would then fail, the reverse check would pass, and the old code would
      # silently flip e.g. `protected:` back to `private:`.
      echo "[fetch] $(basename "${p}") already applied"
    else
      echo "[fetch] FAILED to apply $(basename "${p}") at RawSpeed ${RAWSPEED_REV}" >&2
      echo "[fetch] Pin a RawSpeed revision where the patch set applies cleanly," >&2
      echo "[fetch] and record the substitution in third_party/libraw/PROVENANCE.md." >&2
      exit 1
    fi
  done
fi

# Project-authored LibRaw patches (see patches/libraw/README.md). Rooted at the
# LibRaw tree, applied after LibRaw's own RawSpeed3 patch set. A patch that
# neither applies nor is already applied is a hard failure: silently skipping it
# would produce a tree that PROVENANCE.md misdescribes.
if [ -d "${PROJECT_PATCH_DIR}" ] && [ -d "${DEST}/.git" ]; then
  for p in "${PROJECT_PATCH_DIR}"/*.patch; do
    [ -e "${p}" ] || continue
    if git -C "${DEST}" apply --check "${p}" 2>/dev/null; then
      git -C "${DEST}" apply "${p}"; echo "[fetch] applied project $(basename "${p}")"
    elif git -C "${DEST}" apply --check --reverse "${p}" 2>/dev/null; then
      echo "[fetch] project $(basename "${p}") already applied"
    else
      echo "[fetch] FAILED to apply project patch $(basename "${p}") at LibRaw ${LIBRAW_REV}" >&2
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
