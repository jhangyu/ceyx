"""Unit tests for native/scripts/ci/check_folded_yaml_reverse_sentinel.py
(guard (h), WI-55).

RED SOURCE (per tmp/verify/lead14/batch1-OWNERSHIP.md row WI-55 and
docs/logs/2026-09-13/pyci-ruling-G-guard-plan.md `(h)`): blob `a7840ef6`'s
five phantom-migration entries in `native/scripts/ci/allowlist.py`
(lines 80, 82, 84, 97, 99) -- verified present in history by
`git show a7840ef6:native/scripts/ci/allowlist.py`, taken from history
because `349b5fa4` already retired them at the current tip. The exact
step bodies below are copied VERBATIM from
`git show a7840ef6:.github/workflows/<file>.yml` (heif_dist_android.yml,
jxl_dist_android.yml, jxl_dist_windows.yml, webp_dist_android.yml,
webp_dist_windows.yml) -- not paraphrased, so the test fixture is the
real historical defect, not a synthetic stand-in.

Every RED case builds an ISOLATED temp `.github/workflows/` tree (never
touches this repo's real workflow files) -- same discipline as
test_check_shell_prohibition.py's `_IsolatedRepo`, per the shared-tree red
line (no git operations against the real tree, no mutation of any shared
file).
"""
from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from .. import allowlist
from .. import check_folded_yaml_reverse_sentinel as guard

Entry = allowlist.Entry

_NOT_YET_MIGRATED_REASON = (
    "not yet migrated to native/scripts/ci/ (pyci python-ization campaign; "
    "the *_dist_* workflows are outside the named WI list but Rule 1 scans "
    "every workflow file, so they carry the same generic reason pending a "
    "future WI)"
)

# Verbatim from `git show a7840ef6:.github/workflows/heif_dist_android.yml`
# (the step at that blob's lines 92-102).
_HEIF_DIST_ANDROID_A7840EF6 = """\
jobs:
  build:
    steps:
      - name: Build the HEIF dist (Python carrier)
        shell: bash
        working-directory: ${{ github.workspace }}
        env:
          ANDROID_NDK_HOME: ${{ steps.setup_ndk.outputs.ndk-path }}
        run: >
          python3 native/scripts/ci.py dist-build
          --component heif-stack --platform android --arch arm64-v8a
          --android-ndk "${ANDROID_NDK_HOME}"
          --dist native/third_party/heif-dist-android-arm64-v8a
          --rc-marker HEIF_DIST_ANDROID_RC
"""

# Verbatim from `git show a7840ef6:.github/workflows/jxl_dist_android.yml`
# (blob lines 74-84).
_JXL_DIST_ANDROID_A7840EF6 = """\
jobs:
  build:
    steps:
      - name: Build the libjxl dist (Python carrier)
        shell: bash
        working-directory: ${{ github.workspace }}
        env:
          ANDROID_NDK_HOME: ${{ steps.setup_ndk.outputs.ndk-path }}
        run: >
          python3 native/scripts/ci.py dist-build
          --component libjxl --platform android --arch arm64-v8a
          --android-ndk "${ANDROID_NDK_HOME}"
          --dist native/third_party/libjxl-dist-android-arm64-v8a
          --rc-marker JXL_DIST_ANDROID_RC
"""

# Verbatim from `git show a7840ef6:.github/workflows/jxl_dist_windows.yml`,
# BOTH steps whose (workflow, step_name) key appears in the allowlist at
# that blob: the permanent "Force LF" step (blob lines 56-61, real
# multi-line bash, never folded) and the phantom "Build the libjxl dist"
# step (blob lines 141-148).
_JXL_DIST_WINDOWS_A7840EF6 = """\
jobs:
  build:
    steps:
      - name: Force LF line endings for all git operations
        shell: bash
        run: |
          git config --global core.autocrlf false
          git config --global core.eol lf
          echo "core.autocrlf=$(git config --global core.autocrlf)"
      - name: Build the libjxl dist (Python carrier)
        shell: pwsh
        working-directory: ${{ github.workspace }}
        run: >
          python native/scripts/ci.py dist-build
          --component jxl-stack --platform windows --arch x86_64
          --dist "${{ github.workspace }}/native/third_party/libjxl-dist-windows"
          --rc-marker JXL_DIST_WINDOWS_RC
"""

# Verbatim from `git show a7840ef6:.github/workflows/webp_dist_android.yml`
# (blob lines 75-85).
_WEBP_DIST_ANDROID_A7840EF6 = """\
jobs:
  build:
    steps:
      - name: Build the libwebp dist (Python carrier)
        shell: bash
        working-directory: ${{ github.workspace }}
        env:
          ANDROID_NDK_HOME: ${{ steps.setup_ndk.outputs.ndk-path }}
        run: >
          python3 native/scripts/ci.py dist-build
          --component libwebp --platform android --arch arm64-v8a
          --android-ndk "${ANDROID_NDK_HOME}"
          --dist native/third_party/libwebp-dist-android-arm64-v8a
          --rc-marker WEBP_DIST_ANDROID_RC
"""

# Verbatim from `git show a7840ef6:.github/workflows/webp_dist_windows.yml`,
# BOTH steps whose (workflow, step_name) key appears in the allowlist at
# that blob: the permanent "Force LF" step and the phantom "Build the
# libwebp dist" step (blob lines 108-115; note -- unlike the other four,
# this one's step name has no "(Python carrier)" suffix at this blob).
_WEBP_DIST_WINDOWS_A7840EF6 = """\
jobs:
  build:
    steps:
      - name: Force LF line endings for all git operations
        shell: bash
        run: |
          git config --global core.autocrlf false
          git config --global core.eol lf
          echo "core.autocrlf=$(git config --global core.autocrlf)"
      - name: Build the libwebp dist
        shell: bash
        working-directory: ${{ github.workspace }}
        run: >
          python3 native/scripts/ci.py dist-build
          --component webp-stack --platform windows --arch x86_64
          --dist native/third_party/libwebp-dist-windows
          --rc-marker WEBP_DIST_WINDOWS_RC
"""

# The five phantom entries, EXACTLY as they read at blob a7840ef6
# (`git show a7840ef6:native/scripts/ci/allowlist.py` lines 80, 82, 84,
# 97, 99).
_FIVE_PHANTOM_ENTRIES = (
    Entry("heif_dist_android.yml", "Build the HEIF dist (Python carrier)", _NOT_YET_MIGRATED_REASON),
    Entry("jxl_dist_android.yml", "Build the libjxl dist (Python carrier)", _NOT_YET_MIGRATED_REASON),
    Entry("jxl_dist_windows.yml", "Build the libjxl dist (Python carrier)", _NOT_YET_MIGRATED_REASON),
    Entry("webp_dist_android.yml", "Build the libwebp dist (Python carrier)", _NOT_YET_MIGRATED_REASON),
    Entry("webp_dist_windows.yml", "Build the libwebp dist", _NOT_YET_MIGRATED_REASON),
)

# The two "Force LF" entries co-resident in these same two workflow files
# at a7840ef6 -- at THAT blob they ALSO carried the generic "not yet
# migrated" reason (verified: `git show a7840ef6:native/scripts/ci/
# allowlist.py` lines 81 and 98), but their real body is genuine
# multi-line bash, never a python invocation -- so the guard's silence on
# them is not a coincidence of the CURRENT (tip) reason wording, it holds
# against the historical reason too, purely because the body doesn't
# match. This is "the half that proves the guard discriminates rather
# than just fires" (ruling doc `(h)`, §3).
_TWO_FORCE_LF_ENTRIES_AT_A7840EF6 = (
    Entry("jxl_dist_windows.yml", "Force LF line endings for all git operations", _NOT_YET_MIGRATED_REASON),
    Entry("webp_dist_windows.yml", "Force LF line endings for all git operations", _NOT_YET_MIGRATED_REASON),
)

# The THIRD permanent "Force LF" entry named by the ruling doc as one of
# "the three permanent pre-checkout Force LF entries" (heif_dist_windows.yml),
# at the CURRENT tip's own reason wording (C-G14 #6) -- included so the
# fixture also proves silence under the reason string actually in force
# today, not just under history's.
_HEIF_DIST_WINDOWS_FORCE_LF_AT_TIP = Entry(
    "heif_dist_windows.yml",
    "Force LF line endings for all git operations",
    "C-G14 #6: order-dependent relative to actions/checkout (step index 0, "
    "checkout at index 1); must stay in YAML permanently",
)

_HEIF_DIST_WINDOWS_WORKFLOW = """\
jobs:
  build:
    steps:
      - name: Force LF line endings for all git operations
        shell: bash
        run: |
          git config --global core.autocrlf false
          git config --global core.eol lf
          echo "core.autocrlf=$(git config --global core.autocrlf)"
"""


class _IsolatedWorkflowDir:
    """Builds a throwaway `.github/workflows/`-shaped directory, deleted on
    exit. Never touches this repo's real tree (shared-tree red line)."""

    def __enter__(self):
        self.root = Path(tempfile.mkdtemp(prefix="pyci-wi55-fixture-"))
        self.workflows_dir = self.root / ".github" / "workflows"
        self.workflows_dir.mkdir(parents=True)
        return self

    def __exit__(self, *exc):
        shutil.rmtree(self.root, ignore_errors=True)
        return False

    def write(self, name: str, text: str):
        (self.workflows_dir / name).write_text(text)


class FoldedYamlReverseSentinelTests(unittest.TestCase):
    def test_red_a7840ef6_all_five_phantoms_flagged(self):
        """RED: replaying blob a7840ef6's real workflow bodies against its
        real allowlist entries must ERROR on exactly the five phantom
        migrations -- no more, no fewer."""
        with _IsolatedWorkflowDir() as fixture:
            fixture.write("heif_dist_android.yml", _HEIF_DIST_ANDROID_A7840EF6)
            fixture.write("jxl_dist_android.yml", _JXL_DIST_ANDROID_A7840EF6)
            fixture.write("jxl_dist_windows.yml", _JXL_DIST_WINDOWS_A7840EF6)
            fixture.write("webp_dist_android.yml", _WEBP_DIST_ANDROID_A7840EF6)
            fixture.write("webp_dist_windows.yml", _WEBP_DIST_WINDOWS_A7840EF6)

            entries = _FIVE_PHANTOM_ENTRIES + _TWO_FORCE_LF_ENTRIES_AT_A7840EF6
            parsed_bodies = guard._parsed_run_bodies(
                sorted(fixture.workflows_dir.glob("*.yml"))
            )
            contradictions = guard.find_contradictions(entries, parsed_bodies)

        flagged_keys = {(e.workflow, e.step_name) for e, _ in contradictions}
        expected_keys = {(e.workflow, e.step_name) for e in _FIVE_PHANTOM_ENTRIES}
        self.assertEqual(flagged_keys, expected_keys)
        self.assertEqual(len(contradictions), 5)

    def test_red_a7840ef6_silent_on_force_lf_entries(self):
        """The other half of the same replay: the two 'Force LF' entries
        that carried the SAME generic 'not yet migrated' reason at
        a7840ef6 must NOT be flagged, because their real body is genuine
        multi-line bash, not a python invocation. Proves discrimination,
        not just firing."""
        with _IsolatedWorkflowDir() as fixture:
            fixture.write("jxl_dist_windows.yml", _JXL_DIST_WINDOWS_A7840EF6)
            fixture.write("webp_dist_windows.yml", _WEBP_DIST_WINDOWS_A7840EF6)

            parsed_bodies = guard._parsed_run_bodies(
                sorted(fixture.workflows_dir.glob("*.yml"))
            )
            contradictions = guard.find_contradictions(
                _TWO_FORCE_LF_ENTRIES_AT_A7840EF6, parsed_bodies
            )

        self.assertEqual(contradictions, [])

    def test_silent_on_permanent_force_lf_entry_at_tip_wording(self):
        """Same body, current (tip) reason wording (no longer 'not yet
        migrated' at all) -- still silent, and for a second, independent
        reason (the reason no longer even makes the claim)."""
        with _IsolatedWorkflowDir() as fixture:
            fixture.write("heif_dist_windows.yml", _HEIF_DIST_WINDOWS_WORKFLOW)

            parsed_bodies = guard._parsed_run_bodies(
                sorted(fixture.workflows_dir.glob("*.yml"))
            )
            contradictions = guard.find_contradictions(
                [_HEIF_DIST_WINDOWS_FORCE_LF_AT_TIP], parsed_bodies
            )

        self.assertEqual(contradictions, [])

    def test_green_at_current_allowlist_zero_contradictions(self):
        """GREEN: against the REAL current `.github/workflows/*.yml` tree
        and the REAL current `allowlist.MUST_STAY`, there must be zero
        contradictions -- WI-25 (`349b5fa4`) already retired the five
        phantom entries at the current tip."""
        rc = guard.main()
        self.assertEqual(rc, 0)

    def test_is_single_python_invocation_recognizes_folded_scalar(self):
        """Unit-level check of the fold-awareness itself: a multi-physical-
        line value that YAML has ALREADY folded into one logical line (no
        embedded newline once the sole trailing one is stripped) is
        recognized as a single python invocation -- this is exactly what
        `check_shell_prohibition.py`'s physical-line predicate cannot see."""
        folded = (
            "python3 native/scripts/ci.py dist-build --component heif-stack "
            "--platform android --arch arm64-v8a\n"
        )
        self.assertTrue(guard._is_single_python_invocation(folded))

    def test_is_single_python_invocation_rejects_real_multiline_body(self):
        literal = "git config --global core.autocrlf false\ngit config --global core.eol lf\n"
        self.assertFalse(guard._is_single_python_invocation(literal))

    def test_is_single_python_invocation_rejects_non_python_single_line(self):
        self.assertFalse(guard._is_single_python_invocation("echo hello\n"))

    def test_find_contradictions_silent_when_reason_lacks_sentinel(self):
        """An entry whose body is a single python invocation but whose
        reason does NOT claim 'not yet migrated' is never examined at
        all -- this guard only checks entries that make the specific
        claim it exists to verify."""
        with _IsolatedWorkflowDir() as fixture:
            fixture.write("heif_dist_android.yml", _HEIF_DIST_ANDROID_A7840EF6)
            parsed_bodies = guard._parsed_run_bodies(
                sorted(fixture.workflows_dir.glob("*.yml"))
            )
            entry = Entry(
                "heif_dist_android.yml",
                "Build the HEIF dist (Python carrier)",
                "PERMANENT exemption, unrelated wording",
            )
            contradictions = guard.find_contradictions([entry], parsed_bodies)

        self.assertEqual(contradictions, [])

    def test_find_contradictions_silent_when_step_absent(self):
        """An entry naming a step that no longer exists in any workflow is
        check_shell_prohibition.py's [stale-allowlist] concern, not this
        guard's -- silently skipped here."""
        entry = Entry(
            "does_not_exist.yml", "Nonexistent step", _NOT_YET_MIGRATED_REASON
        )
        contradictions = guard.find_contradictions([entry], {})
        self.assertEqual(contradictions, [])


if __name__ == "__main__":
    unittest.main()
