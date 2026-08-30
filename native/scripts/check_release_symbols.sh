#!/usr/bin/env bash
# Reports whether a shipped binary carries debug information (user ruling Q5,
# 2026-08-30). Exit 0 = clean, 1 = debug info present, 2 = could not check.
#
# "Could not check" is a DISTINCT, NON-ZERO outcome on purpose. A missing
# inspection tool that silently returned success would turn this gate into a
# rubber stamp on every runner where the tool is absent -- which is the exact
# shape of instrument failure this repo has been bitten by repeatedly.
#
# This script NEVER modifies a binary. Auditing and stripping are separate
# operations, so the audit can be run against release artifacts safely.
#
# Format dispatch is by the TARGET's own magic (via `file`), not by the host
# `uname -s`. A single-host cross-platform audit (Task 15 Step 3 downloads
# artifacts for every platform to one machine) would otherwise take the
# wrong inspection branch for every foreign-format file: e.g. running
# `otool` against a Linux ELF .so on a macOS host fails with "is not an
# object file", and a naive `grep -c __DWARF` on that error text reports 0
# matches -- a false CLEAN, not a "could not check". Dispatch on actual file
# format so the checker either uses the right tool or reports CANNOT_CHECK.
set -euo pipefail

TARGET="${1:-}"
if [ -z "${TARGET}" ] || [ ! -f "${TARGET}" ]; then
  echo "usage: check_release_symbols.sh <binary>" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

case "${TARGET}" in
  *.dll|*.DLL)
    # PE: a shipped .pdb beside the DLL is the realistic leak. clang-cl Release
    # does not embed DWARF, so the sidecar is what to look for.
    PDB="${TARGET%.*}.pdb"
    if [ -f "${PDB}" ]; then
      echo "DEBUG_INFO_PRESENT: ${PDB} ships beside ${TARGET}"
      exit 1
    fi
    echo "CLEAN: no .pdb beside ${TARGET}"
    exit 0
    ;;
esac

if ! command -v file > /dev/null 2>&1; then
  echo "CANNOT_CHECK: file(1) unavailable, cannot determine target format" >&2
  exit 2
fi
file -b "${TARGET}" > "${TMP}/filetype.txt" 2>&1
set +e
grep -c 'Mach-O' "${TMP}/filetype.txt" > "${TMP}/is_macho.txt"
IS_MACHO="$(cat "${TMP}/is_macho.txt")"
grep -c 'ELF' "${TMP}/filetype.txt" > "${TMP}/is_elf.txt"
IS_ELF="$(cat "${TMP}/is_elf.txt")"
set -e

if [ "${IS_MACHO}" -gt 0 ]; then
  # Mach-O. Two independent leak vectors on this platform, checked and
  # reported separately:
  #   1. An embedded __DWARF segment (DEBUG_INFO_PRESENT). Note: on modern
  #      ld64 toolchains (observed: Xcode 17 / macOS 24.6) a normally linked
  #      dylib or executable NEVER embeds __DWARF regardless of -g/-gfull --
  #      DWARF stays in the .o files and is only bundled into a separate
  #      .dSYM by `dsymutil`, a step this repo's release builds do not run.
  #      This branch is therefore expected to always read CLEAN in this
  #      repo's builds; it is kept because it is the format's real embedded
  #      debug-info vector and costs nothing to check.
  #   2. N_OSO debug-map stabs in the symbol table (DEBUG_MAP_PRESENT). This
  #      IS the vector that actually leaks on this toolchain: unstripped
  #      Mach-O binaries carry `OSO` stab entries pointing at the absolute
  #      build-machine paths of every source .o/.a member, which both leaks
  #      local filesystem layout and lets a debugger reconstruct symbols
  #      from the original object files. `strip -x -S` removes them.
  if ! command -v otool > /dev/null 2>&1; then
    echo "CANNOT_CHECK: otool unavailable" >&2
    exit 2
  fi
  if ! command -v nm > /dev/null 2>&1; then
    echo "CANNOT_CHECK: nm unavailable" >&2
    exit 2
  fi
  # Output to a file then grep it -- never `otool | grep -q` / `nm | grep -q`,
  # which under pipefail reports 141 when grep exits early on a match. Also
  # capture the tool's OWN exit code immediately: a tool that failed to parse
  # the file (wrong format, corrupt input) prints an error to stdout/stderr
  # and a naive `grep -c PATTERN` on that error text finds 0 matches, which
  # reads exactly like a clean verdict. A non-zero tool exit is CANNOT_CHECK,
  # never CLEAN.
  set +e
  otool -l "${TARGET}" > "${TMP}/loadcmds.txt" 2>&1
  OTOOL_RC=$?
  set -e
  if [ "${OTOOL_RC}" -ne 0 ]; then
    echo "CANNOT_CHECK: otool exited ${OTOOL_RC} inspecting ${TARGET}: $(cat "${TMP}/loadcmds.txt")" >&2
    exit 2
  fi
  set +e
  grep -c '__DWARF' "${TMP}/loadcmds.txt" > "${TMP}/dwarf_count.txt"
  set -e
  DWARF_COUNT="$(cat "${TMP}/dwarf_count.txt")"
  echo "DWARF_SEGMENT_COUNT=${DWARF_COUNT}"

  set +e
  nm -a "${TARGET}" > "${TMP}/nmall.txt" 2>&1
  NM_RC=$?
  set -e
  if [ "${NM_RC}" -ne 0 ]; then
    echo "CANNOT_CHECK: nm exited ${NM_RC} inspecting ${TARGET}: $(cat "${TMP}/nmall.txt")" >&2
    exit 2
  fi
  set +e
  grep -c ' OSO ' "${TMP}/nmall.txt" > "${TMP}/oso_count.txt"
  set -e
  OSO_COUNT="$(cat "${TMP}/oso_count.txt")"
  echo "OSO_STAB_COUNT=${OSO_COUNT}"

  FOUND=0
  if [ "${DWARF_COUNT}" -gt 0 ]; then
    echo "DEBUG_INFO_PRESENT: ${TARGET} carries a __DWARF segment"
    FOUND=1
  fi
  if [ "${OSO_COUNT}" -gt 0 ]; then
    echo "DEBUG_MAP_PRESENT: ${TARGET} carries ${OSO_COUNT} N_OSO debug-map stab(s) leaking build paths"
    FOUND=1
  fi
  if [ "${FOUND}" -gt 0 ]; then
    exit 1
  fi
  echo "CLEAN: ${TARGET}"
  exit 0
fi

if [ "${IS_ELF}" -gt 0 ]; then
  # Prefer native readelf (default on Linux runners, most reliable). Where
  # it's absent -- e.g. auditing a downloaded Linux .so from a macOS host --
  # fall back to llvm-objdump, which macOS's CommandLineTools ship as a
  # cross-format binutils and can read ELF section headers fine. Either way,
  # capture the tool's OWN exit code before trusting its grep count: an error
  # message misread as "zero matches" is a false CLEAN, not evidence.
  SECTIONS_TOOL=""
  SECTIONS_RC=1
  if command -v readelf > /dev/null 2>&1; then
    set +e
    readelf -S "${TARGET}" > "${TMP}/sections.txt" 2>&1
    SECTIONS_RC=$?
    set -e
    SECTIONS_TOOL="readelf -S"
  else
    OBJDUMP=""
    if command -v llvm-objdump > /dev/null 2>&1; then
      OBJDUMP="llvm-objdump"
    elif command -v xcrun > /dev/null 2>&1 && xcrun --find llvm-objdump > /dev/null 2>&1; then
      OBJDUMP="$(xcrun --find llvm-objdump)"
    fi
    if [ -z "${OBJDUMP}" ]; then
      echo "CANNOT_CHECK: neither readelf nor llvm-objdump available" >&2
      exit 2
    fi
    set +e
    "${OBJDUMP}" --section-headers "${TARGET}" > "${TMP}/sections.txt" 2>&1
    SECTIONS_RC=$?
    set -e
    SECTIONS_TOOL="${OBJDUMP} --section-headers"
  fi
  if [ "${SECTIONS_RC}" -ne 0 ]; then
    echo "CANNOT_CHECK: '${SECTIONS_TOOL}' exited ${SECTIONS_RC} inspecting ${TARGET}: $(cat "${TMP}/sections.txt")" >&2
    exit 2
  fi
  # Success-shape guard: a section listing that parsed at all must contain a
  # section named .text (every linked ELF shared object has one). Its
  # absence means the tool ran but didn't actually produce a section table
  # (e.g. printed a warning to stdout instead) -- treat that as CANNOT_CHECK
  # rather than trusting a `.debug_` count of 0 from unparsed output.
  set +e
  grep -c '\.text' "${TMP}/sections.txt" > "${TMP}/text_count.txt"
  set -e
  if [ "$(cat "${TMP}/text_count.txt")" -eq 0 ]; then
    echo "CANNOT_CHECK: '${SECTIONS_TOOL}' output for ${TARGET} does not look like a section table (no .text section found)" >&2
    exit 2
  fi
  set +e
  grep -c '\.debug_' "${TMP}/sections.txt" > "${TMP}/count.txt"
  set -e
  COUNT="$(cat "${TMP}/count.txt")"
  echo "DEBUG_SECTION_COUNT=${COUNT}"
  set +e
  grep -c '\.symtab' "${TMP}/sections.txt" > "${TMP}/symtab_count.txt"
  set -e
  SYMTAB_COUNT="$(cat "${TMP}/symtab_count.txt")"
  echo "SYMTAB_SECTION_COUNT=${SYMTAB_COUNT}"
  if [ "${COUNT}" -gt 0 ]; then
    echo "DEBUG_INFO_PRESENT: ${TARGET} has .debug_* sections"
    exit 1
  fi
  if [ "${SYMTAB_COUNT}" -gt 0 ]; then
    # Weaker signal than .debug_*: an intact symbol table (file(1) calls
    # this "not stripped") leaks less than DWARF but is still worth noting.
    # Not gated on -- `strip --strip-debug` (what this repo's dist scripts
    # run) deliberately keeps .symtab while removing .debug_* sections, so
    # failing the gate on symtab alone would make every correctly-stripped
    # new dist fail.
    echo "SYMTAB_PRESENT_NOTE: ${TARGET} retains a .symtab section (no .debug_* sections; not gated, informational only)"
  fi
  echo "CLEAN: ${TARGET}"
  exit 0
fi

echo "CANNOT_CHECK: unrecognized target format ($(cat "${TMP}/filetype.txt"))" >&2
exit 2
