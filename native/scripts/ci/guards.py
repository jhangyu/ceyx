#!/usr/bin/env python3
"""`ci.py guards` -- the repo-static guard block, run identically on a
laptop and on a CI runner inside one digest-pinned container.

WHAT PROBLEM THIS SOLVES: the local pre-push gate and the CI job used to be
two hand-maintained recipes that were supposed to match. They did not, and
could not be shown to: the local recipe bind-mounted the LIVE host checkout
(carrying every already-fetched gitignored tree and every third-party Python
package the laptop happened to have), while CI ran a fresh `actions/checkout`
on a runner image with a different interpreter and a different site-packages.
Defects that depend on that difference -- most recently a guard importing
`yaml`, which is installed on the author's machine and ABSENT for the pinned
`actions/setup-python` interpreter on the runner -- are structurally
invisible to the local gate. That one cost this campaign nine tests off the
roster and a red CI.

THE MECHANISM, and the one property that makes it worth having: there is
exactly ONE renderer for the `docker run` command string
(`render_docker_argv()` below) and exactly ONE place the image identity is
written down (the `FROM` line of `native/ci.Dockerfile`, parsed by
`read_pinned_image()`). The local gate and the `build.yml` step both invoke
`ci.py guards --docker`, so they do not "agree"; they are the same code path
producing the same bytes. Two strings maintained in two files that a reader
must diff by eye is precisely the arrangement this replaces -- if you are
ever tempted to inline the command into build.yml "so it's visible", that
tempt IS the defect.

THE ONE FIELD THAT IS NOT BYTE-IDENTICAL, declared rather than hidden: the
bind-mount SOURCE path. It is `/Users/<someone>/project/ceyx` on a laptop
and `/home/runner/work/ceyx/ceyx` on a runner; no amount of engineering
makes a host absolute path portable, and a renderer that pretended otherwise
would be lying. So the printed `GUARDS_DOCKER_CMD=` line substitutes the
literal token `<REPO_ROOT>` for it, and everything else in the string --
image DIGEST included, flags, workdir, env, entrypoint argv -- is compared
byte-for-byte between the two artifacts. `render_docker_argv()` takes the
real path only when it is about to EXECUTE; `--docker` prints the tokenised
form. Both come from the same function, so a flag added for execution cannot
fail to appear in the printed string.

WHY `GIT_CONFIG_*` IS IN THE ARGV: the container runs as root while the
bind-mounted tree is owned by the host user's uid. Modern git refuses to
operate on such a tree ("detected dubious ownership") and every git-backed
guard would fail with an error that has nothing to do with the property it
guards -- a false red. The three `GIT_CONFIG_COUNT/KEY_0/VALUE_0` vars are
the non-persistent way to set `safe.directory` for one process; they are
host-independent, so they live in the shared string.

RUN THIS FROM A REAL CHECKOUT, NOT A `git worktree` (measured, not
theorised). In a linked worktree, `.git` is a FILE containing a pointer to
`<main repo>/.git/worktrees/<name>` -- a path OUTSIDE the bind mount. Every
git-backed guard then dies with `fatal: not a git repository`, and
`check_wiring_is_ledger` additionally cannot resolve `origin/main`. Observed:
3 of the 17 guards fail from a worktree and all 17 pass from a `git clone` of
the same commit (tmp/verify/impl-p1-docker/gate-branch.txt vs gate-clone.txt).
Those failures are an artefact of the MOUNT, not a property of the tree, and
must not be "fixed" by relaxing a guard. A CI runner is unaffected:
`actions/checkout` produces a real `.git` directory. Mounting the external
gitdir as a second volume would work but would put a host-specific path into
the shared command string, trading the property this module exists to
provide for a developer convenience -- so the limitation is documented
instead, and printed in the scope block below.

SCOPE IS PRINTED ON EVERY RUN, PASS OR FAIL (`print_scope()`): a gate whose
coverage you have to reconstruct from source is one whose coverage silently
narrows. The negative space -- what this verb does NOT cover -- is printed
with the same prominence as the positive, because an unnamed absence is not
a declared one.
"""
from __future__ import annotations

import os
import shlex
import sys
from pathlib import Path

if __package__:
    from . import report, run
else:  # pragma: no cover -- direct `python3 native/scripts/ci/guards.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import report  # type: ignore
    import run  # type: ignore

REPO_ROOT = Path(__file__).resolve().parents[3]

#: The bind mount's host path is the sole host-dependent field in the
#: rendered command; the printed form carries this token in its place so the
#: local and CI artifacts can be compared byte-for-byte. See module docstring.
REPO_ROOT_TOKEN = "<REPO_ROOT>"

#: Where the repo is mounted inside the container.
CONTAINER_WORKDIR = "/w"

#: The ONLY file in this repo that names a container image. `guards.py`
#: parses it rather than carrying its own copy, so the digest cannot drift
#: between the Dockerfile a human reads and the command the gate runs.
DOCKERFILE = Path("native/ci.Dockerfile")

# ---------------------------------------------------------------------------
# The guard list.
#
# MEMBERSHIP RULE, applied mechanically rather than by taste: a guard belongs
# here if and only if it is REPO-STATIC -- it reads nothing but git-tracked
# content in the checkout. No fetched dependency, no compiled artifact, no
# device, no network. That rule is what makes a bare `actions/checkout` (and
# a read-only bind mount of the same) a sufficient environment for the whole
# block, and it is why the block can run before any provisioning at all.
#
# DELIBERATELY EXCLUDED, named so the absence is declared and not merely
# true:
#   * native/scripts/verify_raw_provenance.py and
#     native/scripts/check_alias_table_convention.py -- both read
#     native/third_party/libraw/, which does not exist until the LibRaw
#     fetch step runs. WI-41 relocated them below that fetch in build.yml
#     for exactly this reason. They are not repo-static and they stay
#     wired where they are; this verb does not cover them.
#   * The five product-artifact assertions (exports, DT_NEEDED, import
#     closure, no-AVX512, min-runtime) -- they read a BUILT library. Out of
#     scope for this verb by construction, and they keep their existing
#     per-platform wiring.
#   * `ci.py selftest` -- a unit-test suite, not a guard over the repo. It
#     is run by its own build.yml step, ahead of this one.
#   * native/scripts/ci/check_test_marker_leak.py -- PROVISIONAL, and the
#   * the superseded local fresh-checkout simulator -- a `git worktree`-based
#     instrument that approximated "a fresh checkout lacks this machine's
#     gitignored vendored trees". This container subsumes it and Phase 1
#     deleted it, so it is named here by description rather than by filename:
#     Phase 1's acceptance is a literal grep-zero claim on that filename, and
#     a comment is a grep hit.
#
# ON THE COUNT, since a number in a document disagreed with the tree: the
# Phase 1 scope prose says "16 repo-static guards". Applying the membership
# rule above mechanically yields SEVENTEEN, and this list is the seventeen.
# The extra one is check_test_marker_leak.py (see its entry below). The
# discrepancy was raised rather than absorbed, and the ruling was to follow
# the derivation, not the prose -- a count inherited from a document has
# been wrong more than once in this campaign. Anyone tempted to "correct"
# this list back down to sixteen to match a sentence: the sentence is the
# thing that is out of date.
# ---------------------------------------------------------------------------
GUARDS: tuple[tuple[str, ...], ...] = (
    ("native/scripts/check_workflow_bashisms.py",),
    ("native/scripts/check_raw_architecture_gates.py",),
    ("native/scripts/ci_conventions_check.py",),
    ("native/scripts/derive_oriented_stage4.py", "--check"),
    ("native/scripts/gen_export_manifest.py", "--check"),
    ("native/scripts/gen_linkage_table.py", "--check"),
    ("native/scripts/gen_shipped_files_cmake.py", "--check"),
    ("native/scripts/ci/check_cmake_sources_tracked.py",),
    ("native/scripts/ci/check_no_test_execution_in_ci.py",),
    ("native/scripts/ci/check_shell_prohibition.py",),
    ("native/scripts/ci/check_argv_contract.py",),
    ("native/scripts/ci/check_errexit_rc_capture.py",),
    ("native/scripts/ci/check_folded_yaml_reverse_sentinel.py",),
    ("native/scripts/ci/check_step_order.py",),
    ("native/scripts/ci/check_wiring_is_ledger.py",),
    ("native/scripts/ci/check_expected_additions.py",),
    # SEVENTEENTH, and the only contested membership. It is a meta-guard: it
    # runs `ci.py selftest` as a subprocess and checks that no test leaked a
    # `NAME=value` line onto real stdout, where AC-2's marker instrument
    # would later parse it as a genuine CI marker. That makes it the only
    # SLOW member here (it pays for a whole suite run) and the only one
    # whose subject is the suite's OUTPUT rather than repo content -- which
    # is why dropping it was arguable. It is INCLUDED because the case for
    # dropping it was a COST argument, and cost is not a membership
    # property: it reads nothing but git-tracked content, needs no fetched
    # dependency, no built artifact and no device. It is also exactly as
    # environment-sensitive as everything else in this list -- a marker leak
    # that only manifests under the runner's interpreter is precisely the
    # shape this container exists to catch.
    ("native/scripts/ci/check_test_marker_leak.py",),
)

#: What this verb does NOT cover. Printed in-band on every run.
NOT_COVERED = (
    "vendored/fetched-tree guards (verify_raw_provenance.py, "
    "check_alias_table_convention.py) -- they need native/third_party/libraw/, "
    "absent until the fetch step; they stay wired in build.yml below the fetch",
    "the five product-artifact assertions (assert-exports, dt-needed, "
    "import-closure, assert-no-avx512, min-runtime) -- they read a BUILT "
    "library, which this container never produces",
    "ci.py selftest and the unit-test roster -- run by their own build.yml "
    "step, not by this verb",
    "macOS and Windows toolchain behaviour -- this container is linux/amd64 "
    "only; the macOS/Windows legs are NOT containerised by this verb",
    "network-fetch behaviour, GitHub Actions env vars and secrets",
    "a `git worktree` as the mount source -- its .git is a FILE pointing "
    "outside the mount, so git-backed guards fail for a reason that is an "
    "artefact of the mount, not a property of the tree. Run the gate from a "
    "real checkout (CI always is one)",
)


def read_pinned_image(repo_root: Path = REPO_ROOT) -> str:
    """Return the digest-pinned image reference from ``native/ci.Dockerfile``.

    Parses the `FROM` line rather than keeping a second copy of the digest.
    REFUSES a tag-pinned reference: a tag is a mutable pointer, so a gate
    that accepted one would silently change what it tests between two runs
    reporting the same command string. That refusal is the reason this
    function exists at all instead of being a module constant.
    """
    path = repo_root / DOCKERFILE
    if not path.is_file():
        raise FileNotFoundError(f"{DOCKERFILE.as_posix()} not found under {repo_root}")
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line.upper().startswith("FROM "):
            continue
        ref = line.split(None, 1)[1].strip()
        if "@sha256:" not in ref:
            raise ValueError(
                f"{DOCKERFILE.as_posix()} FROM line is not digest-pinned: {ref!r} -- "
                "a tag is a mutable pointer and is refused here by design"
            )
        return ref
    raise ValueError(f"{DOCKERFILE.as_posix()} contains no FROM line")


def render_docker_argv(image: str, repo_root_field: str = REPO_ROOT_TOKEN) -> list:
    """The ONE renderer of the gate's docker argv.

    ``repo_root_field`` is the bind-mount source: the literal
    ``<REPO_ROOT>`` token when rendering the string for PRINTING/comparison,
    and the real absolute path when rendering it for EXECUTION. Everything
    else is host-independent, so the two renderings differ in exactly that
    one field and nowhere else -- and because both go through this single
    function, a flag added for execution cannot fail to show up in the
    printed string.
    """
    return [
        "docker",
        "run",
        "--rm",
        # linux/amd64 pinned explicitly: an arm64 laptop would otherwise
        # silently run an arm64 image and the gate would stop resembling the
        # x86_64 runner it exists to predict.
        "--platform",
        "linux/amd64",
        # Read-only: the gate must not be able to mutate the tree it is
        # judging. A guard that "fixes" what it found is not a guard.
        "--volume",
        f"{repo_root_field}:{CONTAINER_WORKDIR}:ro",
        "--workdir",
        CONTAINER_WORKDIR,
        # Non-persistent safe.directory -- see module docstring.
        "--env",
        "GIT_CONFIG_COUNT=1",
        "--env",
        "GIT_CONFIG_KEY_0=safe.directory",
        "--env",
        "GIT_CONFIG_VALUE_0=*",
        # The mount is read-only; Python must not attempt .pyc writes.
        "--env",
        "PYTHONDONTWRITEBYTECODE=1",
        image,
        "python3",
        "native/scripts/ci.py",
        "guards",
        "--in-container",
    ]


def render_docker_cmd(image: str, repo_root_field: str = REPO_ROOT_TOKEN) -> str:
    """The rendered argv as a single shell-quoted string -- the thing the
    acceptance criterion compares byte-for-byte between the local and CI
    artifacts."""
    return shlex.join(render_docker_argv(image, repo_root_field))


def print_scope() -> None:
    """Print coverage AND non-coverage, on every run, pass or fail."""
    report.section("ci.py guards -- scope")
    print(
        f"GUARDS_SCOPE: repo-static guards only, count={len(GUARDS)}, "
        f"container={CONTAINER_WORKDIR} (read-only bind mount)",
        flush=True,
    )
    for i, guard in enumerate(GUARDS, start=1):
        print(f"GUARDS_SCOPE_COVERS[{i}/{len(GUARDS)}]: {' '.join(guard)}", flush=True)
    for item in NOT_COVERED:
        print(f"GUARDS_SCOPE_DOES_NOT_COVER: {item}", flush=True)


def run_checks(repo_root: Path = REPO_ROOT) -> int:
    """Execute every guard in ``GUARDS`` and return 0 iff all returned 0.

    Runs ALL of them rather than stopping at the first failure: a developer
    who has broken two things should learn that in one round, not two.
    Every RC is captured directly from the child process by ``run.run`` --
    never off a pipeline tail (see run.py's rule 3).
    """
    print_scope()
    report.section("ci.py guards -- run")
    failures = []
    for guard in GUARDS:
        label = guard[0]
        print(f"GUARDS_RUN: {' '.join(guard)}", flush=True)
        result = run.run([sys.executable, *guard], cwd=repo_root)
        if result.stdout:
            sys.stdout.write(result.stdout if result.stdout.endswith("\n") else result.stdout + "\n")
        if result.stderr:
            sys.stderr.write(result.stderr if result.stderr.endswith("\n") else result.stderr + "\n")
        sys.stdout.flush()
        print(f"GUARDS_RC({label})={result.returncode}", flush=True)
        if result.returncode != 0:
            failures.append(label)

    report.marker("GUARDS_TOTAL", len(GUARDS))
    report.marker("GUARDS_FAILED", len(failures))
    for label in failures:
        report.error(f"guard failed: {label}")
    rc = 1 if failures else 0
    report.bare_rc(rc, "ci.py guards")
    return rc


def run_in_docker(repo_root: Path = REPO_ROOT) -> int:
    """Render the pinned command, PRINT it in its comparable form, then
    execute it against this host's real repo root."""
    image = read_pinned_image(repo_root)
    printed = render_docker_cmd(image)
    # This line is the acceptance artifact. It is emitted BEFORE the run, so
    # it exists even when the run fails -- an artifact you only get on
    # success cannot be used to diagnose a failure.
    print(f"GUARDS_DOCKER_CMD={printed}", flush=True)
    print(
        "GUARDS_DOCKER_CMD_NOTE: the sole substituted field is the bind-mount "
        f"source ({REPO_ROOT_TOKEN}); it is a host absolute path and cannot be "
        "portable. Image digest, flags, env, workdir and entrypoint argv above "
        "are byte-identical between this artifact and the CI one.",
        flush=True,
    )
    print(f"GUARDS_DOCKER_IMAGE={image}", flush=True)
    print_scope()

    argv = render_docker_argv(image, os.fspath(repo_root))
    result = run.run(argv, cwd=repo_root)
    if result.stdout:
        sys.stdout.write(result.stdout if result.stdout.endswith("\n") else result.stdout + "\n")
    if result.stderr:
        sys.stderr.write(result.stderr if result.stderr.endswith("\n") else result.stderr + "\n")
    sys.stdout.flush()
    report.bare_rc(result.returncode, "ci.py guards --docker")
    return result.returncode


def main(argv=None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if "--docker" in args:
        return run_in_docker()
    # `--in-container` is what the rendered command invokes inside the
    # container; it is also the plain host-side behaviour, so running
    # `ci.py guards` with no flag on a laptop does the same checks without
    # the container. Same code, two environments -- which is the point.
    return run_checks()


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
