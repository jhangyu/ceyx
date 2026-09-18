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
import time
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
# MEMBERSHIP RULE -- apply this, do not pattern-match the list below. A guard
# belongs here if and only if it is REPO-STATIC: it reads nothing but
# git-tracked content in the checkout. No fetched dependency, no compiled
# artifact, no device, no network. That rule is what makes a bare
# `actions/checkout` (and a read-only bind mount of the same) a sufficient
# environment for the whole block, and it is why the block runs before any
# provisioning at all.
#
# IF YOU DELETE A GUARD SCRIPT, DELETE ITS TUPLE ENTRY IN THE SAME COMMIT.
# This list does not discover anything; it is a hand-maintained roster, and a
# roster that goes stale silently is the exact defect class this campaign
# exists to remove. You are not relied upon to remember: `_preflight()` below
# fails LOUDLY and names the retiring phase and owner from
# `RETIREMENT_SCHEDULE`. Six of the seventeen entries are already scheduled
# for removal -- keep that map in step with this tuple.
#
# ADDING a guard is the same discipline in reverse: apply the membership rule,
# add the tuple entry, and if it is NOT repo-static do not add it here at all
# -- wire it in build.yml next to the step that provisions what it needs.
#
# DELIBERATELY EXCLUDED, named so each absence is declared rather than merely
# true:
#   * native/scripts/verify_raw_provenance.py and
#     native/scripts/check_alias_table_convention.py -- both read
#     native/third_party/libraw/, which does not exist until the LibRaw
#     fetch step runs. WI-41 relocated them below that fetch in build.yml
#     for exactly this reason. Not repo-static; they stay wired where they
#     are and this verb does not cover them.
#   * The five product-artifact assertions (exports, DT_NEEDED, import
#     closure, no-AVX512, min-runtime) -- they read a BUILT library. Out of
#     scope by construction; they keep their existing per-platform wiring.
#   * `ci.py selftest` -- a unit-test suite, not a guard over the repo. It
#     has its own build.yml step, ahead of this one.
#   * the superseded local fresh-checkout simulator -- a `git worktree`-based
#     instrument that approximated "a fresh checkout lacks this machine's
#     gitignored vendored trees". This container subsumes it and Phase 1
#     deleted it, so it is named here by description rather than by filename:
#     Phase 1's acceptance is a literal grep-zero claim on that filename, and
#     a comment is a grep hit.
#
# ON THE COUNT: the Phase 1 scope prose says "16 repo-static guards".
# Applying the membership rule mechanically yields SEVENTEEN, and this list is
# the seventeen. The discrepancy was raised rather than absorbed, and the
# ruling (lead18, Ruling 1) was to follow the derivation: a count inherited
# from a document has been wrong more than once in this campaign, and
# acceptance names no count. The seventeenth is check_test_marker_leak.py,
# which has NO build.yml consumer at all -- so adopting 17 does not preserve
# an existing CI behaviour, it ADDS coverage CI never had. Anyone tempted to
# "correct" this list back down to sixteen to match a sentence: the sentence
# is the thing that is out of date.
# ---------------------------------------------------------------------------
GUARDS: tuple[tuple[str, ...], ...] = (
    ("native/scripts/check_workflow_bashisms.py",),
    ("native/scripts/check_raw_architecture_gates.py",),
    ("native/scripts/ci_conventions_check.py",),
    ("native/scripts/derive_oriented_stage4.py", "--check"),
    ("native/scripts/gen_export_manifest.py", "--check"),
    # RETIRES IN PHASE 3 (impl-p3) -- and it is the DANGEROUS shape: Phase 3
    # removes the `--check` MODE, not the script. The file will still be
    # here, so a bare existence check passes and the failure surfaces as an
    # argparse "unrecognized arguments" error from a script that looks
    # perfectly healthy. `_classify_failure()` below catches exactly that.
    ("native/scripts/gen_linkage_table.py", "--check"),
    ("native/scripts/gen_shipped_files_cmake.py", "--check"),
    ("native/scripts/ci/check_cmake_sources_tracked.py",),
    ("native/scripts/ci/check_no_test_execution_in_ci.py",),
    ("native/scripts/ci/check_shell_prohibition.py",),        # RETIRES: Phase 2
    ("native/scripts/ci/check_argv_contract.py",),            # RETIRES: Phase 2
    ("native/scripts/ci/check_errexit_rc_capture.py",),
    ("native/scripts/ci/check_folded_yaml_reverse_sentinel.py",),  # RETIRES: Phase 2
    ("native/scripts/ci/check_step_order.py",),               # RETIRES: Phase 2
    ("native/scripts/ci/check_wiring_is_ledger.py",),         # RETIRES: Phase 2
    # KEPT, despite sitting next to five Phase 2 casualties and being the
    # kind of name that looks like one: the user's 21:40 ruling scoped that
    # deletion list to five, and this is not among them.
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

# Machine-readable twin of the `# RETIRES:` comments in GUARDS above. It
# exists so a STALE ENTRY produces an error naming the phase and the owner
# instead of a generic file-not-found that reads like a broken environment.
# The comments are for the person reading the tuple; this map is for the
# person staring at a red gate at 3am wondering what they broke.
#
# Arithmetic for whoever reads next: 17 - 5 (Phase 2) - 1 (Phase 3) = 11 at
# the end of the migration. Every one of these entries is LIVE until its
# deletion actually lands -- coverage does not drop on a schedule that might
# slip, so nothing here is pre-emptively removed.
RETIREMENT_SCHEDULE: dict = {
    "native/scripts/ci/check_shell_prohibition.py": ("Phase 2", "impl-p2-render-opus"),
    "native/scripts/ci/check_argv_contract.py": ("Phase 2", "impl-p2-render-opus"),
    "native/scripts/ci/check_folded_yaml_reverse_sentinel.py": (
        "Phase 2",
        "impl-p2-render-opus",
    ),
    "native/scripts/ci/check_step_order.py": ("Phase 2", "impl-p2-render-opus"),
    "native/scripts/ci/check_wiring_is_ledger.py": ("Phase 2", "impl-p2-render-opus"),
    # Phase 3 removes the --check MODE; the script itself stays, because
    # native/deps/linkage_table.md names it as its own regenerator.
    "native/scripts/gen_linkage_table.py": ("Phase 3", "impl-p3"),
}

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


def _retirement_note(path: str) -> str:
    """The 'who do I talk to' half of a stale-entry error, or '' if this
    guard has no scheduled retirement."""
    entry = RETIREMENT_SCHEDULE.get(path)
    if entry is None:
        return (
            " This guard has NO scheduled retirement, so this is not expected "
            "roster drift -- it is either an unannounced deletion or a broken "
            "checkout. Do not 'fix' it by removing the entry until you know which."
        )
    phase, owner = entry
    return (
        f" This guard was scheduled for deletion in {phase} (owner: {owner}) and "
        f"its GUARDS entry in native/scripts/ci/guards.py was not removed with it. "
        f"The fix is to delete the tuple entry and its RETIREMENT_SCHEDULE row, "
        f"NOT to weaken or skip the guard block."
    )


def _preflight(repo_root: Path) -> list:
    """Fail loudly and specifically on a stale roster entry, BEFORE running
    anything.

    Six of the seventeen entries are scheduled for deletion by three members
    across two phases. When one of those deletions lands without the matching
    tuple edit, the default failure is `python3: can't open file ...` -- which
    reads like a broken container and sends the reader looking at Docker.
    This turns it into a sentence naming the phase and the owner.
    """
    problems = []
    for guard in GUARDS:
        path = guard[0]
        if not (repo_root / path).is_file():
            problems.append(f"STALE GUARDS ENTRY: {path} does not exist.{_retirement_note(path)}")
    return problems


def _classify_failure(guard: tuple, result) -> str:
    """Turn the argparse-flag variant of roster drift into a named diagnosis.

    The `--check` entries are the trap: Phase 3 removes the MODE, not the
    script, so the file still exists, `_preflight()` is satisfied, and the
    guard dies with `error: unrecognized arguments: --check` -- an error that
    looks like a bug in a healthy script rather than a stale roster. Returns
    '' when the failure is an ordinary guard failure, which is the common
    case and must stay quiet so real findings are not buried.
    """
    flags = [a for a in guard[1:] if a.startswith("-")]
    if not flags:
        return ""
    blob = f"{result.stdout}\n{result.stderr}"
    if "unrecognized arguments" in blob or "invalid choice" in blob:
        path = guard[0]
        return (
            f"STALE GUARDS ENTRY: {path} exists but no longer accepts "
            f"{' '.join(flags)}.{_retirement_note(path)}"
        )
    return ""


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
        retirement = RETIREMENT_SCHEDULE.get(guard[0])
        suffix = f"  [retires: {retirement[0]}, owner {retirement[1]}]" if retirement else ""
        print(
            f"GUARDS_SCOPE_COVERS[{i}/{len(GUARDS)}]: {' '.join(guard)}{suffix}",
            flush=True,
        )
    # Cost: QUALITATIVE here, because this block prints BEFORE anything has
    # been timed. The numbers are emitted after the run by
    # `_print_cost_summary()`, COMPUTED from the same durations printed on the
    # GUARDS_SECONDS lines.
    #
    # This used to carry hard-coded figures and they went stale exactly as you
    # would predict: the text said "21.4s against 2.8s", which was a real
    # measurement from a HOST run, while the artifact it was printed into was
    # a CONTAINER run whose own lines said 49.2s against 8.8s. A narrative
    # number contradicting the mechanical numbers in the same log is the
    # defect class this whole campaign exists to remove, and the reviewer
    # found it by summing the lines this function emits. Hence the rule now
    # enforced by construction rather than by diligence: a summary sentence is
    # DERIVED from the data it summarises, never transcribed alongside it. A
    # recomputed sentence cannot go stale; a transcribed one always can.
    print(
        "GUARDS_SCOPE_COST: check_test_marker_leak.py re-runs the full unit "
        "suite in-process and DOMINATES this block's wall time. Accepted "
        "deliberately (correctness over speed): it is the only guard covering "
        "marker leakage into the job log, and CI has never run it before. "
        "Exact figures for THIS run are computed from this run's own timings "
        "and printed as GUARDS_COST_SUMMARY below -- read that, not a "
        "remembered number, and note that host and container runs differ "
        "substantially.",
        flush=True,
    )
    print(
        f"GUARDS_SCOPE_RETIRING: {len(RETIREMENT_SCHEDULE)} of {len(GUARDS)} entries "
        "are scheduled for deletion by later phases; each stays LIVE until its "
        "deletion actually lands, and a stale entry fails loudly by name",
        flush=True,
    )
    for item in NOT_COVERED:
        print(f"GUARDS_SCOPE_DOES_NOT_COVER: {item}", flush=True)


def _print_cost_summary(durations: list) -> None:
    """Emit the cost summary DERIVED from this run's own measurements.

    ``durations`` is a list of ``(label, seconds)`` -- the very same values
    printed on the GUARDS_SECONDS lines. Nothing here is transcribed, so this
    sentence cannot drift away from the data underneath it, which is the
    failure it exists to prevent: the previous hard-coded version quoted a
    host run's figures inside a container run's artifact and was off by more
    than a factor of two.
    """
    if not durations:
        return
    total = sum(s for _, s in durations)
    slowest_label, slowest = max(durations, key=lambda pair: pair[1])
    others = total - slowest
    # Guard the division: a fast enough machine could in principle floor the
    # rest of the block at 0.0s, and a crash in the cost REPORTER would be an
    # absurd way to fail a green guard run.
    ratio = f"{slowest / others:.1f}x" if others > 0 else "n/a (rest of block ~0s)"
    block_ratio = f"{total / others:.1f}x" if others > 0 else "n/a"
    print(
        f"GUARDS_COST_SUMMARY: slowest={slowest_label} {slowest:.1f}s; "
        f"other_{len(durations) - 1}_combined={others:.1f}s; ratio={ratio}; "
        f"block_total={total:.1f}s; block_vs_without_slowest={block_ratio}",
        flush=True,
    )


def run_checks(repo_root: Path = REPO_ROOT) -> int:
    """Execute every guard in ``GUARDS`` and return 0 iff all returned 0.

    Runs ALL of them rather than stopping at the first failure: a developer
    who has broken two things should learn that in one round, not two.
    Every RC is captured directly from the child process by ``run.run`` --
    never off a pipeline tail (see run.py's rule 3).
    """
    print_scope()

    # Roster health BEFORE any guard runs. A stale entry is a bookkeeping
    # failure, not a guard finding, and conflating the two sends the reader
    # to the wrong place.
    stale = _preflight(repo_root)
    for problem in stale:
        report.error(problem)
    if stale:
        report.marker("GUARDS_STALE_ENTRIES", len(stale))
        report.bare_rc(2, "ci.py guards -- stale roster, nothing executed")
        return 2

    report.section("ci.py guards -- run")
    failures = []
    diagnoses = []
    durations = []
    started = time.monotonic()
    for guard in GUARDS:
        label = guard[0]
        print(f"GUARDS_RUN: {' '.join(guard)}", flush=True)
        guard_started = time.monotonic()
        result = run.run([sys.executable, *guard], cwd=repo_root)
        elapsed = time.monotonic() - guard_started
        if result.stdout:
            sys.stdout.write(result.stdout if result.stdout.endswith("\n") else result.stdout + "\n")
        if result.stderr:
            sys.stderr.write(result.stderr if result.stderr.endswith("\n") else result.stderr + "\n")
        sys.stdout.flush()
        # Wall time per guard, in-band: the block's cost is dominated by one
        # member (check_test_marker_leak re-runs the whole suite), and a
        # reader comparing two runs should be able to see WHERE the time went
        # rather than inferring it.
        print(f"GUARDS_SECONDS({label})={elapsed:.1f}", flush=True)
        durations.append((label, elapsed))
        print(f"GUARDS_RC({label})={result.returncode}", flush=True)
        if result.returncode != 0:
            failures.append(label)
            diagnosis = _classify_failure(guard, result)
            if diagnosis:
                diagnoses.append(diagnosis)

    report.marker("GUARDS_TOTAL", len(GUARDS))
    report.marker("GUARDS_FAILED", len(failures))
    report.marker("GUARDS_TOTAL_SECONDS", f"{time.monotonic() - started:.1f}")
    _print_cost_summary(durations)
    for diagnosis in diagnoses:
        report.error(diagnosis)
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
    # The substitution is declared THREE ways -- that it happened, what the
    # token is, and what it stood for on THIS host -- so that no reader of a
    # green log can mistake `<REPO_ROOT>` for a literal argument that was
    # actually passed to docker. A redacted field that does not announce
    # itself is indistinguishable from a field that was never there.
    print(
        f"GUARDS_DOCKER_CMD_IS_SUBSTITUTED=1 token={REPO_ROOT_TOKEN} "
        f"field=bind-mount-source",
        flush=True,
    )
    print(f"GUARDS_DOCKER_CMD_SUBSTITUTION_VALUE={os.fspath(repo_root)}", flush=True)
    print(
        f"GUARDS_DOCKER_CMD_NOTE: the command string above is NOT verbatim -- "
        f"{REPO_ROOT_TOKEN} is a placeholder standing for the bind-mount source "
        f"printed on the line above, which is a host absolute path and therefore "
        "cannot be portable. Every other field -- image DIGEST, flags, env, "
        "workdir, entrypoint argv -- is byte-identical between this artifact and "
        "the CI one, and that is the equality acceptance tests.",
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
