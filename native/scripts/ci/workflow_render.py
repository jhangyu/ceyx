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

SCOPE TODAY -- READ THIS BEFORE BELIEVING A PASS. This module renders the
THREE Android dist workflows only:

    webp_dist_android.yml  jxl_dist_android.yml  heif_dist_android.yml

It does NOT render the six remaining workflows. `RENDERED` below is the
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
RENDERED = (
    "webp_dist_android.yml",
    "jxl_dist_android.yml",
    "heif_dist_android.yml",
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
