"""Renders `.github/workflows/*.yml` from one description.

WHY THIS EXISTS (Phase 2 of the CI architecture migration, contract
`ci-architecture-migration-contract.md:12`): a per-platform fact that is
spelled once in `targets.py` and again by hand in YAML is two descriptions
of one fact, and every bookkeeping guard in this repo exists only to keep
such a pair synchronized. A generator's output cannot drift from its own
input, so the guards are deleted rather than improved.

COMMENTS LIVE HERE NOW (user ruling 2026-09-18, option (b)). Rendered and
committed YAML carry NO hand-written comments; the incident record they
held is RELOCATED into this module, not deleted. `_MIGRATED_COMMENTS` below
holds every migrated block VERBATIM, keyed by the file and the anchor it
sat at, and `comment_migration_table()` prints that mapping for audit. The
text is stored rather than paraphrased on purpose: a summary would be a
claim that the record survived, and the verbatim text is the record
surviving. Nothing in `_MIGRATED_COMMENTS` is ever emitted.

The safeguard that makes a 2,591-line regeneration diff reviewable is
`strip_comments()`: for every regenerated file, the old committed text with
its pure-comment lines removed must be BYTE-IDENTICAL to the new rendered
text. If that holds, the diff's size stops mattering, because the only
thing that changed is comment lines. If it fails anywhere that is a
FINDING, not a formatting artefact -- a semantic change riding along inside
a diff too large to read is exactly what this check exists to catch.

SCOPE TODAY -- READ THIS BEFORE BELIEVING A PASS. This module renders FIVE
of the eleven workflows:

    webp_dist_android.yml  jxl_dist_android.yml  heif_dist_android.yml
    webp_dist_windows.yml  jxl_dist_windows.yml

It does NOT render the other six. `RENDERED` below is the
explicit, enumerated set, and `ci.py render-workflows --check` compares
exactly those names -- so the assertion can never pass by rendering
nothing, and a name here with no committed counterpart is a failure, not a
silent skip.

WHAT IS DERIVED vs WHAT IS DATA. The arch string (`arm64-v8a`) is NOT
written here: it is read from ``targets.spec("android")["arch_tags"]``,
which is the single source this phase exists to create. Step structure,
step order, action versions, and every `ci.py` invocation come from one
template.

GitHub expression text is held as VERBATIM DATA and never composed from
parts. This module does not parse expressions, so it cannot "helpfully"
normalise `needs['job-id'].result` into dot form -- where a hyphen parses
as SUBTRACTION and silently yields an empty string. If a future extension
finds itself generating an expression from components, STOP and report.
"""

from __future__ import annotations

from . import targets

# The enumerated render set. A name here with no committed file, or a
# mismatch against one, fails `--check`.
#
# DELIBERATELY NOT RENDERED -- heif_dist_windows.yml. This is a declared
# exclusion, not an oversight, and it is not to be "fixed" by adding the
# file here without first re-testing the reason below.
#
# The test lead19 set: render it only if its variable facts genuinely move
# into targets.py, leaving the template holding structure only. They do not.
# Its distinguishing facts are the vcpkg triplet `x64-windows-heif` and the
# `de265`/`aom` feature selection, which are properties of the HEIF-STACK
# COMPONENT, not of the windows PLATFORM. targets.py is keyed by platform
# (linux/windows/macos/android), so putting a component's triplet in the
# `windows` entry would make that file state a fact it does not own -- the
# exact duplicated-description shape this phase exists to remove, recreated
# in the single description itself. Nothing real would move; the template
# would be the file with its values inlined, plus four vcpkg steps
# (bootstrap / install / assert-aom-artifact / export-prefix) that exist on
# no other leg.
#
# Measured, not asserted: 14 steps vs the Windows template's 9, 12 path
# triggers vs 4, and a job-level `env:` block the template has no concept
# of. If a second leg ever needs that vcpkg block, the shared structure
# becomes real and this decision should be revisited.
RENDERED = (
    "webp_dist_android.yml",
    "jxl_dist_android.yml",
    "heif_dist_android.yml",
    "webp_dist_windows.yml",
    "jxl_dist_windows.yml",
)


def strip_comments(text: str) -> str:
    """Drops whole-line comments, leaving everything else byte-exact.

    Only lines whose first non-space character is `#` are removed; nothing
    is reflowed, no blank line is collapsed, no trailing whitespace is
    touched. That narrowness is the point -- this function defines the
    ONLY difference a comment-migration commit is permitted to make, and a
    stripper that also tidied would hide the very changes it is meant to
    expose.

    Verified safe for the rendered set before use: every `#` in those three
    files is a whole-line comment, so there is no inline-hash case where a
    `#` inside a quoted value could be mistaken for one.
    """
    return "".join(
        line for line in text.splitlines(keepends=True) if not line.lstrip().startswith("#")
    )


# Per-workflow data. Identifiers only -- every field here appears in the
# rendered output. Prose lives in _MIGRATED_COMMENTS and is never emitted.
_DIST: dict = {
    "webp_dist_android.yml": {
        "title": "libwebp dist (Android)",
        "short": "webp",
        "dist_prefix": "libwebp",
        "component": "libwebp",
        "rc_marker": "WEBP_DIST_ANDROID_RC",
        "job_name": "libwebp dist (android arm64-v8a, NDK, static, encode+decode+mux)",
        "build_step_name": "Build the libwebp dist (Python carrier)",
        "timeout_minutes": 45,
        "apt_step": False,
    },
    "jxl_dist_android.yml": {
        "title": "libjxl dist (Android)",
        "short": "jxl",
        "dist_prefix": "libjxl",
        "component": "libjxl",
        "rc_marker": "JXL_DIST_ANDROID_RC",
        "job_name": "libjxl dist (android arm64-v8a, NDK, static, encode+decode)",
        "build_step_name": "Build the libjxl dist (Python carrier)",
        "timeout_minutes": 45,
        "apt_step": False,
    },
    "heif_dist_android.yml": {
        "title": "HEIF dist (Android)",
        "short": "heif",
        "dist_prefix": "heif",
        "component": "heif-stack",
        "rc_marker": "HEIF_DIST_ANDROID_RC",
        "job_name": "libheif + libde265 + kvazaar + aom dist (android arm64-v8a, NDK, encode+decode)",
        "build_step_name": "Build the HEIF dist (Python carrier)",
        "timeout_minutes": 90,
        "apt_step": True,
    },
}

# ---------------------------------------------------------------------------
# THE RELOCATED INCIDENT RECORD (user ruling 2026-09-18, option (b)).
#
# Every comment block that used to live in the three rendered workflows,
# VERBATIM, keyed by the file it came from and the anchor it sat at. This
# structure is NEVER emitted. It exists so the claim "the comments were
# relocated, not deleted" can be CHECKED -- `comment_migration_table()`
# prints file, anchor, first line and line count for each entry, and the
# full text is right here to read.
#
# These are not decoration. They record real incidents: the 08-23
# exit-code lesson (a harness-reported status lying about which process's
# code it forwarded), WI-40's folded-scalar defect (a `run: >` scalar
# reading as five physical lines to a physical-line classifier), and the
# reason `include-hidden-files: true` is mandatory (upload-artifact v4
# excludes dotfiles, and the `.pins` stamp is what a staleness digest check
# depends on).
# ---------------------------------------------------------------------------
_MIGRATED_COMMENTS: dict = {
    "webp_dist_android.yml": [
        (
            "header: between `name:` and `on:`",
            """\
DISPATCH-ONLY BY DESIGN, same rationale as webp_dist_windows.yml /
heif_dist_windows.yml: the dist is a pinned, reviewed input that is
COMMITTED to native/third_party/libwebp-dist-android-arm64-v8a/, not rebuilt per
push.

ci/** is included so the workflow can be verified from a bootstrap branch:
a workflow file only becomes dispatchable once it exists on a ref that runs
it.

`workflow_call` lets build.yml invoke this leg on demand (Option B, task
CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
wired into build.yml's per-push platform matrix for the same "moving
target under a committed dist" reason documented on the Windows dist legs
-- see build.yml's "Windows third-party dist legs" comment block, which
this leg's build.yml entry sits directly beside and shares the run_dists
input with (DP-3 ruled: one boolean, no per-platform split).""",
        ),
        (
            "before step `Set up Android NDK`",
            """\
Pinned NDK revision, same choice and rationale as android_build.yml:
not the runner image's preinstalled NDK, which can drift silently
when GitHub bumps the image.""",
        ),
        (
            "before step `Install Ninja`",
            """\
CMake ships with the runner image; Ninja does not reliably.
(WI-31: collapsed into ci.py's `provision ninja` -- emits no marker,
same as the shell it replaces.)""",
        ),
        (
            "before step `Build the libwebp dist (Python carrier)`",
            """\
Single carrier entry point (build_deps.py), same contract as every
other dist producer. RC captured on the line immediately after the
command, echoed from the step itself -- a harness-reported status has
lied about which process's exit code it forwarded before on this
project (08-23 lesson). (WI-31: collapsed into ci.py's `dist-build`
-- rc read from the child process object, not a shell variable;
`--rc-marker WEBP_DIST_ANDROID_RC` reproduces the same
`WEBP_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        ),
        (
            "inside step `Build the libwebp dist (Python carrier)`, above `run:`",
            """\
WI-40: collapsed from a `run: >` folded scalar to one physical
line -- workflow_scan.code_lines() parses run: bodies by raw
physical line and does not fold block scalars, so this step read
as 5 physical lines and failed Rule-1 even though WI-31 already
migrated it to the one-line python3 carrier. YAML-parsed `run`
string verified byte-identical before/after; formatting only.""",
        ),
        (
            "before step `List the produced dist (complete)`",
            """\
Unfiltered on purpose: a '*.a'-filtered listing looks like a full
inventory while silently omitting the headers. (WI-31: `if: always()`
stays on the step; `dist-list` replaces the `find | sort` pipeline
and refuses to succeed silently on a missing or empty dist -- a
deliberate tightening over the old pipeline, which printed nothing
and exited 0 on an empty tree.)""",
        ),
        (
            "before step `Upload the dist`",
            "Canonical artifact name <component>-<platform>-<arch> (rule C4).",
        ),
        (
            "inside step `Upload the dist`, above `include-hidden-files:`",
            """\
actions/upload-artifact v4 excludes dotfiles by default; the
carrier's .pins stamp (and the dist-local .gitignore) must ship
with this artifact so a committed dist tree carries the pin
record CI-T8's staleness digest check depends on.""",
        ),
    ],
    "jxl_dist_android.yml": [
        (
            "header: between `name:` and `on:`",
            """\
DISPATCH-ONLY BY DESIGN, same rationale as jxl_dist_windows.yml /
heif_dist_windows.yml: the dist is a pinned, reviewed input that is
COMMITTED to native/third_party/libjxl-dist-android-arm64-v8a/, not rebuilt per
push.

ci/** is included so the workflow can be verified from a bootstrap branch:
a workflow file only becomes dispatchable once it exists on a ref that runs
it.

`workflow_call` lets build.yml invoke this leg on demand (Option B, task
CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
wired into build.yml's per-push platform matrix -- see build.yml's
"Windows third-party dist legs" comment block, which this leg's build.yml
entry sits directly beside and shares the run_dists input with (DP-3
ruled: one boolean, no per-platform split).""",
        ),
        (
            "before step `Set up Android NDK`",
            """\
Pinned NDK revision, same choice and rationale as android_build.yml:
not the runner image's preinstalled NDK, which can drift silently
when GitHub bumps the image.""",
        ),
        (
            "before step `Install Ninja`",
            """\
CMake ships with the runner image; Ninja does not reliably.
(WI-31: collapsed into ci.py's `provision ninja` -- emits no marker,
same as the shell it replaces.)""",
        ),
        (
            "before step `Build the libjxl dist (Python carrier)`",
            """\
Single carrier entry point (build_deps.py), same contract as every
other dist producer. RC captured on the line immediately after the
command, echoed from the step itself -- a harness-reported status has
lied about which process's exit code it forwarded before on this
project (08-23 lesson). (WI-31: collapsed into ci.py's `dist-build`
-- rc read from the child process object, not a shell variable;
`--rc-marker JXL_DIST_ANDROID_RC` reproduces the same
`JXL_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        ),
        (
            "inside step `Build the libjxl dist (Python carrier)`, above `run:`",
            """\
WI-40: collapsed from a `run: >` folded scalar to one physical
line -- workflow_scan.code_lines() parses run: bodies by raw
physical line and does not fold block scalars, so this step read
as 5 physical lines and failed Rule-1 even though WI-31 already
migrated it to the one-line python3 carrier. YAML-parsed `run`
string verified byte-identical before/after; formatting only.""",
        ),
        (
            "before step `List the produced dist (complete)`",
            """\
Unfiltered on purpose: a filtered listing looks like a full inventory
while silently omitting some artifact class. (WI-31: `if: always()`
stays on the step; `dist-list` replaces the `find | sort` pipeline
and refuses to succeed silently on a missing or empty dist -- a
deliberate tightening over the old pipeline, which printed nothing
and exited 0 on an empty tree.)""",
        ),
        (
            "before step `Upload the dist`",
            "Canonical artifact name <component>-<platform>-<arch> (rule C4).",
        ),
        (
            "inside step `Upload the dist`, above `include-hidden-files:`",
            """\
actions/upload-artifact v4 excludes dotfiles by default; the
carrier's .pins stamp (and the dist-local .gitignore) must ship
with this artifact so a committed dist tree carries the pin
record CI-T8's staleness digest check depends on.""",
        ),
    ],
    "heif_dist_android.yml": [
        (
            "header: between `name:` and `on:`",
            """\
DISPATCH-ONLY BY DESIGN, same rationale as heif_dist_windows.yml: this job
builds the libheif/libde265/kvazaar/aom distribution that is then COMMITTED
to native/third_party/heif-dist-android-arm64-v8a/, exactly as the macOS/Windows
dists are. It is not part of the per-commit Android build: the dist is a
pinned, reviewed input (upstream tarball + SHA-256 + a fixed flag set), and
rebuilding it on every push would make the shipped bytes a moving target
under an LGPL source-availability obligation.

ci/** is included so the workflow can be verified from a bootstrap branch:
a workflow file only becomes dispatchable once it exists on a ref that runs
it.

`workflow_call` lets build.yml invoke this leg on demand (Option B, task
CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
wired into build.yml's per-push platform matrix -- see build.yml's
"Windows third-party dist legs" comment block, which this leg's build.yml
entry sits directly beside and shares the run_dists input with (DP-3
ruled: one boolean, no per-platform split).

timeout-minutes is 90, not the 45 used by the webp/jxl Android dist legs:
the desktop (Windows) HEIF dist takes ~40min on clang-cl alone, this leg
additionally cross-compiles libheif + libde265 + kvazaar + aom for arm64
via the NDK, so the budget is doubled with margin rather than measured and
tightened after the fact.""",
        ),
        (
            "before step `Set up Android NDK`",
            """\
Pinned NDK revision, same choice and rationale as android_build.yml:
not the runner image's preinstalled NDK, which can drift silently
when GitHub bumps the image.""",
        ),
        (
            "before step `Install Ninja`",
            """\
CMake ships with the runner image; Ninja does not reliably.
(WI-31: pip-install + version print collapsed into ci.py's
`provision ninja` -- emits no marker, same as the shell it replaces.)""",
        ),
        (
            "before step `Install build prerequisites (apt)`",
            """\
cmake/nasm/etc. that the aom/kvazaar/de265 CMake subbuilds may need on
the host side of a cross-compile; mirrors android_build.yml's apt
prerequisite step. (WI-31: collapsed into ci.py's `provision apt`
-- emits no marker, same as the shell it replaces.)""",
        ),
        (
            "before step `Build the HEIF dist (Python carrier)`",
            """\
Single carrier entry point (build_deps.py), same contract as every
other dist producer, invoked through the "heif-stack" group exactly
as heif_dist_windows.yml does. RC captured on the line immediately
after the command, echoed from the step itself -- a harness-reported
status has lied about which process's exit code it forwarded before
on this project (08-23 lesson). (WI-31: collapsed into ci.py's
`dist-build` -- rc read from the child process object, not a shell
variable; `--rc-marker HEIF_DIST_ANDROID_RC` reproduces the same
`HEIF_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        ),
        (
            "inside step `Build the HEIF dist (Python carrier)`, above `run:`",
            """\
WI-40: collapsed from a `run: >` folded scalar to one physical
line -- workflow_scan.code_lines() parses run: bodies by raw
physical line and does not fold block scalars, so this step read
as 5 physical lines and failed Rule-1 even though WI-31 already
migrated it to the one-line python3 carrier. YAML-parsed `run`
string verified byte-identical before/after; formatting only.""",
        ),
        (
            "before step `List the produced dist (complete)`",
            """\
Complete listing, deliberately unfiltered -- same rationale as
heif_dist_windows.yml's equivalent step. (WI-31: `if: always()` stays
on the step, not the module; `dist-list` replaces the
`find | sort` pipeline and refuses to succeed silently on a missing
or empty dist -- a deliberate tightening over the old pipeline,
which printed nothing and exited 0 on an empty tree.)""",
        ),
        (
            "before step `Upload the dist`",
            "Canonical artifact name <component>-<platform>-<arch> (rule C4).",
        ),
        (
            "inside step `Upload the dist`, above `include-hidden-files:`",
            """\
actions/upload-artifact v4 excludes dotfiles by default; the
carrier's .pins stamp (and the dist-local .gitignore) must ship
with this artifact so a committed dist tree carries the pin
record CI-T8's staleness digest check depends on.""",
        ),
    ],
    "webp_dist_windows.yml": [
        (
            'header: between `name:` and `on:`',
            """\
DISPATCH-ONLY BY DESIGN, same rationale as heif_dist_windows.yml: the dist is
a pinned, reviewed input that is COMMITTED, not rebuilt per push.

ci/** is included so the workflow can be verified from a bootstrap branch:
a workflow file only becomes dispatchable once it exists on a ref that runs it.

Round 6 (Plan B): `workflow_call` was ADDED alongside the existing triggers so
build.yml can invoke this leg on demand; workflow_dispatch is KEPT and remains
the primary entry point. Sole producer of the committed
native/third_party/libwebp-dist-windows tree — see heif_dist_windows.yml for
the full rationale and for why this leg is not in build.yml's per-push matrix.""",
        ),
        (
            'before step `Force LF line endings for all git operations`',
            """\
MUST run BEFORE actions/checkout, same reason as heif_dist_windows.yml:
Git for Windows ships core.autocrlf=true system-wide, and a checkout
done before this writes shell scripts with CRLF, which Git-Bash then
fails to execute with an opaque error.""",
        ),
        (
            'before step `Install Ninja`',
            """\
CMake ships with the runner image; Ninja does not reliably. pip's ninja
wheel is a pinned, deterministic way to get it on PATH without choco.

WI-32 (push 8b): migrated to `ci.py provision ninja` (WI-29's
`provision.ninja()`, identical body shared with windows_build.yml and
the other *_dist_*.yml twins). `shell:` deliberately left unset --
preserves the pre-migration default (`pwsh` on windows-latest).""",
        ),
        (
            'before step `Set up MSVC developer environment (x64)`',
            """\
Ninja + clang-cl need the MSVC headers/libs/linker on PATH, INCLUDE and
LIB; this action is CI's equivalent of vcvars64.bat.""",
        ),
        (
            'before step `Locate clang-cl`',
            """\
WI-32 (push 8b): migrated to `ci.py provision locate-clang-cl`, which
reuses `windows_toolchain.locate_clang_cl` (WI-24) rather than a
second implementation.

KNOWN DIVERGENCE, recorded rather than silently carried (identical to
jxl_dist_windows.yml's note): the reused function's not-found message
is "::error::clang-cl not found on the runner.
native/cmake/pipeline.cmake requires it (cl.exe has no
-ffp-contract=off equivalent)." (windows_toolchain.py:52-54), longer
than this file's pre-migration "::error::clang-cl not found on the
runner." Both `::error::`-prefixed, same trigger condition, text-only
difference -- invisible to marker-diff since it only fires on an
already-red step.""",
        ),
        (
            'before step `Build the libwebp dist`',
            """\
Migrated off build_libwebp_dist_windows.sh (2026-09-01 contract item
11 / ENTRY-POINT RULE): the carrier is native/scripts/deps/win_webp_dist.py,
exposed as `build_deps.py build webp-stack`. build_libwebp_dist_windows.sh
was DELETED in round 3 once this carrier was proven green on a real
Windows run -- see win_webp_dist_test.py for the transcription test
frozen against its last revision.

WI-32 (push 8b): migrated to `ci.py dist-build`. `--dist` keeps this
file's pre-migration RELATIVE form (`native/third_party/...`, not the
`${{ github.workspace }}/...` absolute form jxl/heif use) -- confirmed
against WI-29 that `--dist` passes through unmodified, so the two
argv forms are preserved as each file already had them rather than
silently normalised to match. `shell: bash` KEPT (this is the one
Windows carrier step written in bash, not pwsh -- see this file's
header). The pre-migration `set +e` / `RC=$?` / `set -e` dance existed
only to survive Actions' default `bash -eo pipefail` long enough to
capture $?; `dist_build.py` needs no such workaround because it reads
`result.returncode` from the `subprocess` object Python's own `run.py`
returns, adjacent to the call, never from a shell variable -- the
same guarantee the old dance was manually establishing.
WI-40: collapsed from a `run: >` folded scalar to one physical line
-- workflow_scan.code_lines() parses run: bodies by raw physical
line and does not fold block scalars, so this step read as 4
physical lines and failed Rule-1 even though it was already the
one-line python3 `dist-build` carrier WI-32 wrote. YAML-parsed
`run` string verified byte-identical before/after; formatting
only. `shell: bash` and the relative `--dist` form are both
untouched -- see this step's WI-32 comment block above for why.""",
        ),
        (
            'before step `List the produced dist (complete)`',
            """\
Unfiltered on purpose: a '*.lib'-filtered listing looks like a full
inventory while silently omitting the headers.

WI-32 (push 8b): migrated to `ci.py dist-list` (WI-29) -- same
deliberate tightening as jxl_dist_windows.yml's equivalent step
(refuses to succeed silently on a missing/empty tree).""",
        ),
        (
            'before step `Upload the dist`',
            """\
Canonical artifact name <component>-<platform>-<arch> (round 6); the
packaged directory keeps its committed tracked path.""",
        ),
    ],
    "jxl_dist_windows.yml": [
        (
            'header: between `name:` and `on:`',
            """\
DISPATCH-ONLY BY DESIGN, same rationale as heif_dist_windows.yml /
webp_dist_windows.yml: the dist is a pinned, reviewed input that is
COMMITTED, not rebuilt per push.

timeout-minutes is 90, not the 45-60 used by the other two Windows dists:
libjxl is the heaviest of the three builds and highway's SIMD dispatch is
the most compiler-sensitive part of it.

ci/** is included so the workflow can be verified from a bootstrap branch:
a workflow file only becomes dispatchable once it exists on a ref that runs it.

Round 6 (Plan B): `workflow_call` was ADDED alongside the existing triggers so
build.yml can invoke this leg on demand; workflow_dispatch is KEPT and remains
the primary entry point. Sole producer of the committed
native/third_party/libjxl-dist-windows tree — see heif_dist_windows.yml for
the full rationale and for why this leg is not in build.yml's per-push matrix.

2026-09-01 (contract item 10 / ENTRY-POINT RULE): built by the PYTHON
CARRIER (native/scripts/deps/win_jxl_dist.py via
`build_deps.py build jxl-stack`), not by build_libjxl_dist_windows.sh --
same migration heif_dist_windows.yml already made for the HEIF dist. The
path trigger below follows suit: it names the carrier module paths, the
same S2/S3 trigger-coverage rationale heif_dist_windows.yml documents (a
carrier change must be able to trigger the leg that proves it).
build_libjxl_dist_windows.sh was retired in round 2 once the carrier-built
dist was committed and green (its transcription test is frozen in
win_jxl_dist_test.py); it was never a trigger path here since it was no
longer consumed by this job.""",
        ),
        (
            'before step `Force LF line endings for all git operations`',
            """\
MUST run BEFORE actions/checkout, same reason as the sibling dists:
Git for Windows ships core.autocrlf=true system-wide, and a checkout
done before this writes shell scripts with CRLF, which Git-Bash then
fails to execute with an opaque error.""",
        ),
        (
            'before step `Install Ninja`',
            """\
CMake ships with the runner image; Ninja does not reliably. pip's ninja
wheel is a pinned, deterministic way to get it on PATH without choco.

WI-32 (push 8b): migrated to `ci.py provision ninja`, the identical
body already extracted by WI-29 (provision.py's `ninja()` -- shared
verbatim with windows_build.yml and the other five *_dist_*.yml
twins, no per-caller variant). Emits no marker, matching this step's
pre-migration silence. `shell:` deliberately left unset, same as the
pre-migration body -- the runner's default (`pwsh` on windows-latest)
is preserved rather than switched to `bash` to match the Android
dist twins' convention (WI-31); those run on a linux runner where
`bash` already was the default, so their explicit key changed
nothing, but stating it here would be a real interpreter change.""",
        ),
        (
            'before step `Set up MSVC developer environment (x64)`',
            """\
Ninja + clang-cl need the MSVC headers/libs/linker on PATH, INCLUDE and
LIB; this action is CI's equivalent of vcvars64.bat.""",
        ),
        (
            'before step `Locate clang-cl`',
            """\
WI-32 (push 8b): migrated to `ci.py provision locate-clang-cl`, which
reuses `windows_toolchain.locate_clang_cl` (WI-24's port for
windows_build.yml's identical body) rather than a second
implementation, per dist_build.py's own twin-diff finding.

KNOWN DIVERGENCE, recorded rather than silently carried: the reused
function's not-found message is
"::error::clang-cl not found on the runner. native/cmake/pipeline.cmake
requires it (cl.exe has no -ffp-contract=off equivalent)."
(windows_toolchain.py:52-54), longer than this file's pre-migration
"::error::clang-cl not found on the runner." (no trailing sentence).
Both are `::error::`-prefixed and fire on the identical condition; the
difference is text-only. Failure-path text is invisible to marker-diff
(it is only emitted when the step is already red), so this could not
have been caught by a green gate -- flagged here instead of silently
adopting shared code with a changed string.""",
        ),
        (
            'before step `Build the libjxl dist (Python carrier)`',
            """\
Built by the PYTHON CARRIER, not by build_libjxl_dist_windows.sh.

`shell: pwsh` rather than bash is the substantive change here, not a
style preference -- same rationale as heif_dist_windows.yml's
equivalent step: under Git-Bash every path-shaped argument is
eligible for MSYS rewriting, and Git-Bash can put an MSYS Python on
PATH, which deps/run.py refuses to run under by design. pwsh + native
Windows Python means argv reaches CreateProcessW unmodified.

$LASTEXITCODE is captured on the line IMMEDIATELY after the command
and echoed from the step itself, mirroring heif_dist_windows.yml.

Invoked through the SINGLE CARRIER ENTRY POINT (build_deps.py), the
ENTRY-POINT RULE (2026-09-01 contract item 10): every migrated
capability lands as a build_deps.py subcommand, `jxl-stack` here.
WI-32 (push 8b): migrated to `ci.py dist-build`, WI-29's frozen
carrier invocation. `--dist` passes the same absolute
`${{ github.workspace }}/...` form this step already used --
dist_build.py forwards it unmodified, no normalisation (confirmed
against WI-29 before this migration). rc is read from the child
process object inside dist_build.py (adjacent to the call, never a
shell variable), and `--rc-marker JXL_DIST_WINDOWS_RC` reproduces
`JXL_DIST_WINDOWS_RC=<rc>` byte-for-byte, replacing
`$RC = $LASTEXITCODE; Write-Host "JXL_DIST_WINDOWS_RC=$RC"`.
`shell: pwsh` kept -- same reason as before the migration: Git-Bash
is eligible to MSYS-rewrite path-shaped argv and can put an MSYS
Python on PATH, which `deps/run.py` refuses to run under.
WI-40: collapsed from a `run: >` folded scalar to one physical line
-- workflow_scan.code_lines() parses run: bodies by raw physical
line and does not fold block scalars, so this step read as 4
physical lines and failed Rule-1 even though it was already the
one-line python `dist-build` carrier WI-32 wrote. YAML-parsed `run`
string verified byte-identical before/after (including the
un-expanded `${{ github.workspace }}` text); formatting only.
`shell: pwsh` and the `python` (not `python3`) spelling are both
untouched -- see this step's WI-32 comment block above for why.""",
        ),
        (
            'before step `List the produced dist (complete)`',
            """\
Unfiltered on purpose: a '*.lib'-filtered listing looks like a full
inventory while silently omitting the headers.

WI-32 (push 8b): migrated to `ci.py dist-list` (WI-29), which
replaces `find | sort` and additionally refuses to succeed silently
on a missing/empty dist tree (exits 1 with an `::error::` line where
the old pipeline printed nothing and exited 0) -- a deliberate
tightening, not a defect, per dist_build.py's own docstring.""",
        ),
        (
            'before step `Upload the dist`',
            """\
Canonical artifact name <component>-<platform>-<arch> (round 6); the
packaged directory keeps its committed tracked path.""",
        ),
    ],
}

# ---------------------------------------------------------------------------
# WINDOWS DIST LEGS. A SECOND TEMPLATE, NOT A PARAMETER OF THE ANDROID ONE.
# The step sequences genuinely differ: Windows needs an LF-line-endings step
# BEFORE checkout, an MSVC developer-environment action and a clang-cl
# locator, and has no NDK step; its dist directory carries no arch suffix and
# its upload sets no include-hidden-files. Forcing one template to cover both
# would mean a parameter per difference, which is how a renderer stops being
# readable. Two templates, one data shape.
# ---------------------------------------------------------------------------
_WIN_DIST: dict = {
    "webp_dist_windows.yml": {
        "title": "libwebp dist (Windows)",
        "short": "webp",
        "dist_prefix": "libwebp",
        "component": "webp-stack",
        "rc_marker": "WEBP_DIST_WINDOWS_RC",
        "job_name": "libwebp dist (windows x86_64, clang-cl, static, encode+decode+mux)",
        "build_step_name": "Build the libwebp dist",
        "timeout_minutes": 45,
        "path_trigger": "native/scripts/deps/win_webp_dist.py",
        "build_shell": "bash",
        "python_exe": "python3",
        # RELATIVE --dist, deliberately. WI-32 preserved each file's
        # pre-migration argv form rather than normalising the two to match,
        # having confirmed --dist passes through unmodified. jxl/heif use
        # the ${{ github.workspace }}-absolute form. Rendering them the same
        # would be a silent normalisation of exactly what WI-32 declined to
        # normalise.
        "dist_absolute": False,
    },
    "jxl_dist_windows.yml": {
        "title": "libjxl dist (Windows)",
        "short": "jxl",
        "dist_prefix": "libjxl",
        "component": "jxl-stack",
        "rc_marker": "JXL_DIST_WINDOWS_RC",
        "job_name": "libjxl dist (windows x86_64, clang-cl, static, encode+decode)",
        "build_step_name": "Build the libjxl dist (Python carrier)",
        "timeout_minutes": 90,
        "path_trigger": "native/scripts/deps/**",
        # pwsh + `python`, not bash + `python3`: this leg's carrier step was
        # written in the runner's default shell and never migrated to bash.
        "build_shell": "pwsh",
        "python_exe": "python",
        "dist_absolute": True,
    },
}

# The apt prerequisites step, present only on the HEIF leg.
_APT_STEP = """\

      - name: Install build prerequisites (apt)
        shell: bash
        run: python3 native/scripts/ci.py provision apt --packages cmake ninja-build build-essential
"""


def _android_arch() -> str:
    """The Android arch tag, from the single source (targets.py).

    Single-tag leg by construction: ``requires_arch`` is False for android
    and ``targets._validate()`` refuses to let those two keys disagree, so
    indexing [0] here cannot silently pick one of several.
    """
    return targets.spec("android")["arch_tags"][0]


def render(name: str) -> str:
    """Returns the full text of one rendered workflow, newline-terminated.

    Emits NO comments (user ruling, option (b)). The prose that used to sit
    in this output is in `_MIGRATED_COMMENTS`, verbatim, and is never read
    by this function.
    """
    if name in _WIN_DIST:
        return _render_windows_dist(name)
    try:
        d = _DIST[name]
    except KeyError:
        raise KeyError(
            f"unknown workflow {name!r}; rendered set: {', '.join(RENDERED)}"
        ) from None

    arch = _android_arch()
    dist = f"native/third_party/{d['dist_prefix']}-dist-android-{arch}"
    artifact = f"{d['dist_prefix']}-dist-android-{arch}"
    apt = _APT_STEP if d["apt_step"] else ""

    return f"""\
name: {d['title']}

on:
  workflow_call:
  workflow_dispatch:
  push:
    branches:
      - "ci/**"
    paths:
      - "native/scripts/deps/**"
      - "native/scripts/build_deps.py"
      - "native/deps/manifest.toml"
      - "native/deps/arch_map.toml"
      - ".github/workflows/{name}"

concurrency:
  group: {d['short']}-dist-android-${{{{ github.ref }}}}
  cancel-in-progress: true

jobs:
  build-{d['short']}-dist:
    name: {d['job_name']}
    runs-on: ubuntu-latest
    timeout-minutes: {d['timeout_minutes']}
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.11"

      - name: Set up Android NDK
        id: setup_ndk
        uses: nttld/setup-ndk@v1
        with:
          ndk-version: r27c
          add-to-path: false

      - name: Install Ninja
        shell: bash
        run: python3 native/scripts/ci.py provision ninja
{apt}
      - name: {d['build_step_name']}
        shell: bash
        working-directory: ${{{{ github.workspace }}}}
        env:
          ANDROID_NDK_HOME: ${{{{ steps.setup_ndk.outputs.ndk-path }}}}
        run: |
          python3 native/scripts/ci.py dist-build --component {d['component']} --platform android --arch {arch} --android-ndk "${{ANDROID_NDK_HOME}}" --dist {dist} --rc-marker {d['rc_marker']}

      - name: List the produced dist (complete)
        if: always()
        shell: bash
        working-directory: ${{{{ github.workspace }}}}
        run: python3 native/scripts/ci.py dist-list --dist {dist}

      - name: Upload the dist
        uses: actions/upload-artifact@v4
        with:
          name: {artifact}
          path: ${{{{ github.workspace }}}}/{dist}
          if-no-files-found: error
          retention-days: 7
          include-hidden-files: true
"""


def _render_windows_dist(name: str) -> str:
    d = _WIN_DIST[name]
    arch = targets.spec("windows")["arch_tags"][0]
    dist = f"native/third_party/{d['dist_prefix']}-dist-windows"
    dist_arg = f'"${{{{ github.workspace }}}}/{dist}"' if d["dist_absolute"] else dist
    return f"""\
name: {d['title']}

on:
  workflow_call:
  workflow_dispatch:
  push:
    branches:
      - "ci/**"
    paths:
      - "{d['path_trigger']}"
      - "native/scripts/build_deps.py"
      - ".github/workflows/{name}"

concurrency:
  group: {d['short']}-dist-windows-${{{{ github.ref }}}}
  cancel-in-progress: true

jobs:
  build-{d['short']}-dist:
    name: {d['job_name']}
    runs-on: windows-latest
    timeout-minutes: {d['timeout_minutes']}
    steps:
      - name: Force LF line endings for all git operations
        shell: bash
        run: |
          git config --global core.autocrlf false
          git config --global core.eol lf
          echo "core.autocrlf=$(git config --global core.autocrlf)"

      - name: Checkout
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.11"

      - name: Install Ninja
        run: python native/scripts/ci.py provision ninja

      - name: Set up MSVC developer environment (x64)
        uses: ilammy/msvc-dev-cmd@v1
        with:
          arch: x64

      - name: Locate clang-cl
        shell: bash
        run: python3 native/scripts/ci.py provision locate-clang-cl --github-path "$GITHUB_PATH"

      - name: {d['build_step_name']}
        shell: {d['build_shell']}
        working-directory: ${{{{ github.workspace }}}}
        run: |
          {d['python_exe']} native/scripts/ci.py dist-build --component {d['component']} --platform windows --arch {arch} --dist {dist_arg} --rc-marker {d['rc_marker']}

      - name: List the produced dist (complete)
        if: always()
        shell: bash
        working-directory: ${{{{ github.workspace }}}}
        run: python3 native/scripts/ci.py dist-list --dist {dist}

      - name: Upload the dist
        uses: actions/upload-artifact@v4
        with:
          name: {d['dist_prefix']}-dist-windows-{arch}
          path: ${{{{ github.workspace }}}}/{dist}
          if-no-files-found: error
          retention-days: 7
"""


def render_all() -> dict:
    """{filename: rendered text} for every name in RENDERED."""
    return {name: render(name) for name in RENDERED}


# Classification of each migrated block, keyed by the anchor it sat at
# (lead18 requirement). WHY IT EXISTS: under the user's option (b), every
# INSTRUCTION that lived in a committed workflow comment stops existing
# there. Those are not narration -- they tell a future editor what to do,
# and losing one costs a WRONG ACTION rather than lost context. An auditor
# should check instruction and evidence blocks EXHAUSTIVELY and sample the
# narration, which is impossible if the table treats all 25 alike.
#
# The cut, per lead19's binding definitions:
#   instruction  tells a future editor to do or not do something. Tie-break
#                is BINDING: torn between instruction and narration ->
#                instruction, because losing narration costs context while
#                losing an instruction costs a wrong action.
#   evidence     names a measurement, artefact path, run id, work item or
#                commit that justifies a setting, so it can be re-checked
#   narration    commands nothing AND cites nothing
#
# MEASURED RESULT: narration is EMPTY here. All 25 blocks either command or
# cite -- the seven I had first called narration each cite a work item
# (WI-31) or a ruling (CI-T6, DP-3), which makes them evidence by the
# definition above, not narration. That is not a classification failure: a
# comment in a CI workflow exists because someone had to justify or defend
# a setting, so "explains nothing actionable and cites nothing" describes
# almost no surviving comment. The practical consequence is that audit here
# is EXHAUSTIVE over all 25; what the classification still buys is naming
# WHICH 12 can cause a wrong action if lost.
#
# The canonical instruction case is not in this file at all: build.yml's
# `fetch-depth: 0` under guards-container carries "DELETE THIS LINE when it
# goes". That is the difference between a removable setting and one nobody
# dares touch.
_COMMENT_KIND: dict = {
    # cites Option B / task CI-T6 and the DP-3 ruling -> evidence, not
    # narration: it names the decisions that justify the trigger set.
    "header: between `name:` and `on:`": "evidence",
    "before step `Set up Android NDK`": "instruction",
    # both cite WI-31 (the shell-to-ci.py collapse) as the justification
    # for the step's present form.
    "before step `Install Ninja`": "evidence",
    "before step `Install build prerequisites (apt)`": "evidence",
    # --- Windows dist leg anchors ---
    # MUST run before checkout or Git-for-Windows' system-wide
    # core.autocrlf=true writes CRLF shell scripts that Git-Bash cannot
    # execute: a step-ORDER directive, not an explanation.
    "before step `Force LF line endings for all git operations`": "instruction",
    # names the action that supplies MSVC headers/libs to clang-cl.
    "before step `Set up MSVC developer environment (x64)`": "evidence",
    # records WI-32's reuse of windows_toolchain.locate_clang_cl and the
    # known error-text divergence it accepted.
    "before step `Locate clang-cl`": "evidence",
    "before step `List the produced dist (complete)`": "instruction",
    "before step `Upload the dist`": "instruction",
    "inside step `Upload the dist`, above `include-hidden-files:`": "instruction",
}


def _kind_for(anchor: str) -> str:
    if anchor in _COMMENT_KIND:
        return _COMMENT_KIND[anchor]
    # The two per-component anchors carry the build step's name, so they
    # cannot be spelled as fixed keys. Both are EVIDENCE: the first records
    # the 08-23 exit-code incident (a harness-reported status lying about
    # which process's code it forwarded) and states the marker is
    # reproduced byte-for-byte; the second records WI-40's folded-scalar
    # defect and states the run string was verified identical before/after.
    if anchor.startswith("inside step `Build the "):
        return "evidence"
    if anchor.startswith("before step `Build the "):
        return "evidence"
    raise KeyError(
        f"unclassified comment anchor {anchor!r} -- classify it rather than "
        "letting it default; an unclassified block is one an auditor will "
        "sample instead of checking"
    )


def comment_migration_table(name: str) -> list:
    """Audit rows for one file: (anchor, kind, line_count, first_line).

    The user's protective requirement on option (b): the incident record is
    RELOCATED, not deleted, and this table is how that is checked rather
    than asserted. `kind` is lead18's addition -- see `_COMMENT_KIND`. The
    full verbatim text sits in `_MIGRATED_COMMENTS`.
    """
    rows = []
    for anchor, text in _MIGRATED_COMMENTS.get(name, []):
        lines = text.splitlines()
        rows.append((anchor, _kind_for(anchor), len(lines), lines[0] if lines else ""))
    return rows


def step_names(text: str) -> list:
    """Every `- name:` step label in a workflow, in order.

    A rendered file that silently drops a step still satisfies
    `rendered == committed` once the rendered output is committed -- the
    assertion is satisfied by the very file that lost the step. Comparing
    step NAMES, and never counts, is what makes that disappearance visible.
    """
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("- name:"):
            out.append(s[len("- name:"):].strip())
    return out
