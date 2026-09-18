"""Renders `.github/workflows/*.yml` from one description.

WHY THIS EXISTS (Phase 2 of the CI architecture migration, contract
`ci-architecture-migration-contract.md:12`): a per-platform fact that is
spelled once in `targets.py` and again by hand in YAML is two descriptions
of one fact, and every bookkeeping guard in this repo exists only to keep
such a pair synchronized. A generator's output cannot drift from its own
input, so the guards are deleted rather than improved.

SCOPE TODAY -- READ THIS BEFORE BELIEVING A PASS. This module renders the
THREE Android dist workflows only:

    webp_dist_android.yml  jxl_dist_android.yml  heif_dist_android.yml

It does NOT render the six remaining workflows (the four platform legs,
`build.yml`, and the three Windows dist legs). `RENDERED` below is the
explicit, enumerated set, and `ci.py render-workflows --check` compares
exactly those names -- so the assertion can never pass by rendering
nothing, and a file added here without a committed counterpart is a
failure, not a silent skip.

WHAT IS DERIVED vs WHAT IS DATA. The arch string (`arm64-v8a`) is NOT
written here: it is read from ``targets.spec("android")["arch_tags"]``,
which is the single source this phase exists to create. Step structure,
step order, action versions, and every `ci.py` invocation are derived from
one template. Per-workflow English prose is DATA, held verbatim in
``_DIST``: these comments record real incidents (the 08-23 exit-code
lesson, WI-40's folded-scalar defect) and paraphrasing them to shorten this
file would destroy the only record of why the steps look like this. A
comment that survives byte-identical is a comment nobody has to re-earn.
"""

from __future__ import annotations

from . import targets

# The enumerated render set. A name here with no committed file, or a
# mismatch against one, fails `--check`. Extending this set is how future
# pushes bring the remaining workflows under the renderer.
RENDERED = (
    "webp_dist_android.yml",
    "jxl_dist_android.yml",
    "heif_dist_android.yml",
)

# Per-workflow data. Identifiers are derived from `short` and `dist_prefix`;
# the `*_comment` fields are verbatim prose, reproduced exactly.
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
        "header_comment": """\
# DISPATCH-ONLY BY DESIGN, same rationale as webp_dist_windows.yml /
# heif_dist_windows.yml: the dist is a pinned, reviewed input that is
# COMMITTED to native/third_party/libwebp-dist-android-arm64-v8a/, not rebuilt per
# push.
#
# ci/** is included so the workflow can be verified from a bootstrap branch:
# a workflow file only becomes dispatchable once it exists on a ref that runs
# it.
#
# `workflow_call` lets build.yml invoke this leg on demand (Option B, task
# CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
# wired into build.yml's per-push platform matrix for the same "moving
# target under a committed dist" reason documented on the Windows dist legs
# -- see build.yml's "Windows third-party dist legs" comment block, which
# this leg's build.yml entry sits directly beside and shares the run_dists
# input with (DP-3 ruled: one boolean, no per-platform split).""",
        "ninja_comment": """\
      # CMake ships with the runner image; Ninja does not reliably.
      # (WI-31: collapsed into ci.py's `provision ninja` -- emits no marker,
      # same as the shell it replaces.)""",
        "build_comment": """\
      # Single carrier entry point (build_deps.py), same contract as every
      # other dist producer. RC captured on the line immediately after the
      # command, echoed from the step itself -- a harness-reported status has
      # lied about which process's exit code it forwarded before on this
      # project (08-23 lesson). (WI-31: collapsed into ci.py's `dist-build`
      # -- rc read from the child process object, not a shell variable;
      # `--rc-marker WEBP_DIST_ANDROID_RC` reproduces the same
      # `WEBP_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        "list_comment": """\
      # Unfiltered on purpose: a '*.a'-filtered listing looks like a full
      # inventory while silently omitting the headers. (WI-31: `if: always()`
      # stays on the step; `dist-list` replaces the `find | sort` pipeline
      # and refuses to succeed silently on a missing or empty dist -- a
      # deliberate tightening over the old pipeline, which printed nothing
      # and exited 0 on an empty tree.)""",
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
        "header_comment": """\
# DISPATCH-ONLY BY DESIGN, same rationale as jxl_dist_windows.yml /
# heif_dist_windows.yml: the dist is a pinned, reviewed input that is
# COMMITTED to native/third_party/libjxl-dist-android-arm64-v8a/, not rebuilt per
# push.
#
# ci/** is included so the workflow can be verified from a bootstrap branch:
# a workflow file only becomes dispatchable once it exists on a ref that runs
# it.
#
# `workflow_call` lets build.yml invoke this leg on demand (Option B, task
# CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
# wired into build.yml's per-push platform matrix -- see build.yml's
# "Windows third-party dist legs" comment block, which this leg's build.yml
# entry sits directly beside and shares the run_dists input with (DP-3
# ruled: one boolean, no per-platform split).""",
        "ninja_comment": """\
      # CMake ships with the runner image; Ninja does not reliably.
      # (WI-31: collapsed into ci.py's `provision ninja` -- emits no marker,
      # same as the shell it replaces.)""",
        "build_comment": """\
      # Single carrier entry point (build_deps.py), same contract as every
      # other dist producer. RC captured on the line immediately after the
      # command, echoed from the step itself -- a harness-reported status has
      # lied about which process's exit code it forwarded before on this
      # project (08-23 lesson). (WI-31: collapsed into ci.py's `dist-build`
      # -- rc read from the child process object, not a shell variable;
      # `--rc-marker JXL_DIST_ANDROID_RC` reproduces the same
      # `JXL_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        "list_comment": """\
      # Unfiltered on purpose: a filtered listing looks like a full inventory
      # while silently omitting some artifact class. (WI-31: `if: always()`
      # stays on the step; `dist-list` replaces the `find | sort` pipeline
      # and refuses to succeed silently on a missing or empty dist -- a
      # deliberate tightening over the old pipeline, which printed nothing
      # and exited 0 on an empty tree.)""",
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
        "header_comment": """\
# DISPATCH-ONLY BY DESIGN, same rationale as heif_dist_windows.yml: this job
# builds the libheif/libde265/kvazaar/aom distribution that is then COMMITTED
# to native/third_party/heif-dist-android-arm64-v8a/, exactly as the macOS/Windows
# dists are. It is not part of the per-commit Android build: the dist is a
# pinned, reviewed input (upstream tarball + SHA-256 + a fixed flag set), and
# rebuilding it on every push would make the shipped bytes a moving target
# under an LGPL source-availability obligation.
#
# ci/** is included so the workflow can be verified from a bootstrap branch:
# a workflow file only becomes dispatchable once it exists on a ref that runs
# it.
#
# `workflow_call` lets build.yml invoke this leg on demand (Option B, task
# CI-T6); `workflow_dispatch` is kept as the primary human entry point. Not
# wired into build.yml's per-push platform matrix -- see build.yml's
# "Windows third-party dist legs" comment block, which this leg's build.yml
# entry sits directly beside and shares the run_dists input with (DP-3
# ruled: one boolean, no per-platform split).
#
# timeout-minutes is 90, not the 45 used by the webp/jxl Android dist legs:
# the desktop (Windows) HEIF dist takes ~40min on clang-cl alone, this leg
# additionally cross-compiles libheif + libde265 + kvazaar + aom for arm64
# via the NDK, so the budget is doubled with margin rather than measured and
# tightened after the fact.""",
        "ninja_comment": """\
      # CMake ships with the runner image; Ninja does not reliably.
      # (WI-31: pip-install + version print collapsed into ci.py's
      # `provision ninja` -- emits no marker, same as the shell it replaces.)""",
        "build_comment": """\
      # Single carrier entry point (build_deps.py), same contract as every
      # other dist producer, invoked through the "heif-stack" group exactly
      # as heif_dist_windows.yml does. RC captured on the line immediately
      # after the command, echoed from the step itself -- a harness-reported
      # status has lied about which process's exit code it forwarded before
      # on this project (08-23 lesson). (WI-31: collapsed into ci.py's
      # `dist-build` -- rc read from the child process object, not a shell
      # variable; `--rc-marker HEIF_DIST_ANDROID_RC` reproduces the same
      # `HEIF_DIST_ANDROID_RC=<rc>` marker byte-for-byte.)""",
        "list_comment": """\
      # Complete listing, deliberately unfiltered -- same rationale as
      # heif_dist_windows.yml's equivalent step. (WI-31: `if: always()` stays
      # on the step, not the module; `dist-list` replaces the
      # `find | sort` pipeline and refuses to succeed silently on a missing
      # or empty dist -- a deliberate tightening over the old pipeline,
      # which printed nothing and exited 0 on an empty tree.)""",
    },
}

# The apt prerequisites step, present only on the HEIF leg (its aom/kvazaar/
# de265 CMake subbuilds need host-side tooling the other two do not).
_APT_STEP = """\

      # cmake/nasm/etc. that the aom/kvazaar/de265 CMake subbuilds may need on
      # the host side of a cross-compile; mirrors android_build.yml's apt
      # prerequisite step. (WI-31: collapsed into ci.py's `provision apt`
      # -- emits no marker, same as the shell it replaces.)
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
    tags = targets.spec("android")["arch_tags"]
    return tags[0]


def render(name: str) -> str:
    """Returns the full text of one rendered workflow, newline-terminated."""
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

{d['header_comment']}
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

      # Pinned NDK revision, same choice and rationale as android_build.yml:
      # not the runner image's preinstalled NDK, which can drift silently
      # when GitHub bumps the image.
      - name: Set up Android NDK
        id: setup_ndk
        uses: nttld/setup-ndk@v1
        with:
          ndk-version: r27c
          add-to-path: false

{d['ninja_comment']}
      - name: Install Ninja
        shell: bash
        run: python3 native/scripts/ci.py provision ninja
{apt}
{d['build_comment']}
      - name: {d['build_step_name']}
        shell: bash
        working-directory: ${{{{ github.workspace }}}}
        env:
          ANDROID_NDK_HOME: ${{{{ steps.setup_ndk.outputs.ndk-path }}}}
        # WI-40: collapsed from a `run: >` folded scalar to one physical
        # line -- workflow_scan.code_lines() parses run: bodies by raw
        # physical line and does not fold block scalars, so this step read
        # as 5 physical lines and failed Rule-1 even though WI-31 already
        # migrated it to the one-line python3 carrier. YAML-parsed `run`
        # string verified byte-identical before/after; formatting only.
        run: |
          python3 native/scripts/ci.py dist-build --component {d['component']} --platform android --arch {arch} --android-ndk "${{ANDROID_NDK_HOME}}" --dist {dist} --rc-marker {d['rc_marker']}

{d['list_comment']}
      - name: List the produced dist (complete)
        if: always()
        shell: bash
        working-directory: ${{{{ github.workspace }}}}
        run: python3 native/scripts/ci.py dist-list --dist {dist}

      # Canonical artifact name <component>-<platform>-<arch> (rule C4).
      - name: Upload the dist
        uses: actions/upload-artifact@v4
        with:
          name: {artifact}
          path: ${{{{ github.workspace }}}}/{dist}
          if-no-files-found: error
          retention-days: 7
          # actions/upload-artifact v4 excludes dotfiles by default; the
          # carrier's .pins stamp (and the dist-local .gitignore) must ship
          # with this artifact so a committed dist tree carries the pin
          # record CI-T8's staleness digest check depends on.
          include-hidden-files: true
"""


def render_all() -> dict:
    """{filename: rendered text} for every name in RENDERED."""
    return {name: render(name) for name in RENDERED}


def step_names(text: str) -> list:
    """Every `- name:` step label in a workflow, in order.

    Used by the acceptance evidence for this phase. A rendered file that
    silently drops a step still satisfies `rendered == committed` once the
    rendered output is committed -- the assertion is satisfied by the very
    file that lost the step. Comparing step NAMES before and after, and
    never counts, is what makes that disappearance visible.
    """
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("- name:"):
            out.append(s[len("- name:"):].strip())
    return out
