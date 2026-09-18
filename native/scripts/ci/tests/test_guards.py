"""Tests for native/scripts/ci/guards.py (Phase 1: the containerised
repo-static guard block).

WHAT THESE TESTS ARE ACTUALLY DEFENDING. The verb's value rests on three
claims, and each one has a failure mode that looks exactly like success from
the outside, so each gets a test that would go red if the claim quietly
stopped holding:

  1. The pin is a DIGEST, never a tag. A tag-pinned gate still runs, still
     prints a command string, still passes -- while silently testing a
     different image than it did yesterday. `read_pinned_image()` refuses
     tags; `RefusesATagTest` proves the refusal fires rather than trusting
     that it is written down.
  2. The local string and the CI string come from ONE renderer. The way this
     claim dies is not with an error, it is with someone inlining a
     "equivalent" command into build.yml; so `render_docker_argv()` is
     tested for the property that its printed and executed renderings differ
     in EXACTLY the bind-mount source and nothing else. A flag added for
     execution only would fail this.
  3. Every listed guard is real and git-tracked. A typo'd path would make
     the gate greener, not redder -- `run.run` returns 127 for a missing
     interpreter target, but a path that exists on the author's disk while
     being untracked is the WI-44 defect this whole campaign is about, and
     it is invisible in a bind mount of the live tree.
"""
from __future__ import annotations

import importlib.util
import io
import shlex
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from .. import guards
from .. import run as ci_run  # the ONE sanctioned subprocess wrapper; a raw
# `import subprocess` here is caught by check_no_test_execution_in_ci.py's
# `[subprocess-outside-run]` rule -- which it duly caught when this file
# first used one, inside the very container this module is about.

_REPO_ROOT = Path(__file__).resolve().parents[4]


def _load_ci_entry():
    """Load `native/scripts/ci.py` BY PATH.

    `import ci` cannot be used here and the reason is a trap worth naming:
    `native/scripts/` contains BOTH `ci.py` (the entry point) and `ci/` (the
    package). A regular package shadows a same-named module, so `import ci`
    after putting `native/scripts/` on `sys.path` silently binds the
    PACKAGE -- and a test asserting "the guards verb is registered" would
    then fail with a confusing AttributeError about `build_parser`, or, far
    worse, pass against the wrong object if the package ever grew a
    same-named symbol. Loading by explicit file path removes the ambiguity.
    `native/scripts/` still goes on `sys.path` because ci.py's own dispatch
    does `import ci.<module>` and needs the package importable.
    """
    entry = _REPO_ROOT / "native" / "scripts" / "ci.py"
    sys.path.insert(0, str(entry.parent))
    try:
        spec = importlib.util.spec_from_file_location("ceyx_ci_entry_under_test", entry)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class DockerfilePinTest(unittest.TestCase):
    def test_dockerfile_from_line_is_digest_pinned(self):
        """The committed Dockerfile itself -- not a fixture -- must carry a
        digest. This is the one test that fails if a future edit "simplifies"
        the FROM line back to `python:3.11`."""
        text = (_REPO_ROOT / guards.DOCKERFILE).read_text(encoding="utf-8")
        from_lines = [ln.strip() for ln in text.splitlines() if ln.strip().upper().startswith("FROM ")]
        self.assertEqual(len(from_lines), 1, f"expected exactly one FROM line, got {from_lines}")
        self.assertIn("@sha256:", from_lines[0])

    def test_read_pinned_image_returns_that_digest(self):
        image = guards.read_pinned_image(_REPO_ROOT)
        self.assertTrue(image.startswith("python@sha256:"), image)
        # 64 hex chars after the algorithm prefix -- a truncated digest is
        # not a digest.
        self.assertEqual(len(image.split("@sha256:")[1]), 64, image)


class ReadPinnedImageRefusalTest(unittest.TestCase):
    """Red-first on the pin rule: each refusal is exercised, not asserted in
    prose."""

    def _dockerfile_with(self, body: str) -> Path:
        tmp = Path(tempfile.mkdtemp(prefix="ceyx-guards-test."))
        target = tmp / guards.DOCKERFILE
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(body, encoding="utf-8")
        return tmp

    def test_refuses_a_tag_pinned_from_line(self):
        root = self._dockerfile_with("FROM python:3.11\n")
        with self.assertRaises(ValueError) as ctx:
            guards.read_pinned_image(root)
        self.assertIn("not digest-pinned", str(ctx.exception))

    def test_refuses_a_dockerfile_with_no_from_line(self):
        root = self._dockerfile_with("# comment only\nENV A=1\n")
        with self.assertRaises(ValueError) as ctx:
            guards.read_pinned_image(root)
        self.assertIn("no FROM line", str(ctx.exception))

    def test_refuses_a_missing_dockerfile(self):
        tmp = Path(tempfile.mkdtemp(prefix="ceyx-guards-test."))
        with self.assertRaises(FileNotFoundError):
            guards.read_pinned_image(tmp)


class SingleSourceRenderingTest(unittest.TestCase):
    """The acceptance property: one renderer, one string, digest included."""

    def test_printed_and_executed_renderings_differ_in_exactly_the_mount(self):
        image = guards.read_pinned_image(_REPO_ROOT)
        printed = guards.render_docker_argv(image)
        executed = guards.render_docker_argv(image, "/home/runner/work/ceyx/ceyx")
        self.assertEqual(len(printed), len(executed))
        differing = [
            (a, b) for a, b in zip(printed, executed) if a != b
        ]
        self.assertEqual(
            len(differing),
            1,
            f"exactly one field may be host-dependent; got {differing}",
        )
        self.assertIn(guards.REPO_ROOT_TOKEN, differing[0][0])
        self.assertIn("/home/runner/work/ceyx/ceyx", differing[0][1])

    def test_two_hosts_render_byte_identical_strings(self):
        """Two different hosts, same renderer, same bytes -- this is the
        contract's 'byte-identical including digest' claim, stated as an
        executable comparison instead of an eyeball diff."""
        image = guards.read_pinned_image(_REPO_ROOT)
        a = guards.render_docker_cmd(image)
        b = guards.render_docker_cmd(image)
        self.assertEqual(a.encode("utf-8"), b.encode("utf-8"))
        self.assertIn(image, a)
        self.assertIn("@sha256:", a)

    def test_rendered_command_carries_no_mutable_tag(self):
        image = guards.read_pinned_image(_REPO_ROOT)
        cmd = guards.render_docker_cmd(image)
        self.assertNotIn("python:3.11", cmd)

    def test_mount_is_read_only_and_platform_is_pinned(self):
        """Both are load-bearing: a writable mount lets a "guard" repair what
        it found, and an unpinned platform silently runs arm64 on an Apple
        laptop while CI runs amd64 -- the gate would then stop predicting
        CI, which is its only job."""
        image = guards.read_pinned_image(_REPO_ROOT)
        argv = guards.render_docker_argv(image)
        mounts = [argv[i + 1] for i, a in enumerate(argv) if a == "--volume"]
        self.assertEqual(len(mounts), 1, argv)
        self.assertTrue(mounts[0].endswith(":ro"), mounts[0])
        self.assertIn("--platform", argv)
        self.assertEqual(argv[argv.index("--platform") + 1], "linux/amd64")

    def test_entrypoint_reenters_this_same_verb(self):
        image = guards.read_pinned_image(_REPO_ROOT)
        argv = guards.render_docker_argv(image)
        tail = argv[argv.index(image) + 1:]
        self.assertEqual(tail, ["python3", "native/scripts/ci.py", "guards", "--in-container"])


class GuardListTest(unittest.TestCase):
    def test_every_listed_guard_exists_and_is_git_tracked(self):
        """Existence alone is the WI-44 trap: a path can be on the author's
        disk and absent from `git ls-files`, and a read-only bind mount of
        the live tree would still run it happily while a fresh checkout
        could not. Both properties are asserted."""
        tracked = ci_run.run(["git", "ls-files"], cwd=_REPO_ROOT)
        self.assertEqual(tracked.returncode, 0, tracked.stderr)
        tracked_set = set(tracked.stdout.split())
        for guard in guards.GUARDS:
            path = guard[0]
            with self.subTest(guard=path):
                self.assertTrue((_REPO_ROOT / path).is_file(), f"{path} missing on disk")
                self.assertIn(path, tracked_set, f"{path} exists but git does not track it")

    def test_guard_list_has_no_duplicates(self):
        self.assertEqual(len(set(guards.GUARDS)), len(guards.GUARDS))

    def test_the_contested_seventeenth_guard_is_included(self):
        """The Phase 1 scope prose says "16 repo-static guards"; deriving the
        list from the tree yields 17, and the ruling was to follow the
        derivation rather than the sentence. This pins that outcome: if a
        later reader trims the list back to 16 to match the prose, this goes
        red and makes them read the reasoning instead of quietly shrinking
        the gate. Shrinking a gate to match a document is the failure this
        whole campaign keeps paying for.

        Count history: 17 -> 18 when Phase 2 added `ci.py
        render-workflows --check`, then 18 -> 13 when Phase 2 DELETED its
        five synchronizer guards. That second move is a shrink, and it is
        the one this test is built to interrogate -- so read the paragraph
        above before concluding it is fine. It is fine here for a reason
        the paragraph does not cover: those five were removed by a USER
        RULING (2026-09-18 21:40) that named them individually, not to
        make a number match a document. The membership this test actually
        pins (check_test_marker_leak.py) is asserted separately above and
        is unchanged."""
        listed = {g[0] for g in guards.GUARDS}
        self.assertIn("native/scripts/ci/check_test_marker_leak.py", listed)
        self.assertEqual(len(guards.GUARDS), 13)

    def test_artifact_dependent_guards_are_excluded(self):
        """The membership rule is 'repo-static'. These two read
        native/third_party/libraw/, which does not exist until the fetch
        step; WI-41 moved them below it in build.yml for that reason. If a
        future edit sweeps them in here "for completeness", the container
        gate goes red for a reason unrelated to any property it guards."""
        listed = {g[0] for g in guards.GUARDS}
        for excluded in (
            "native/scripts/verify_raw_provenance.py",
            "native/scripts/check_alias_table_convention.py",
        ):
            self.assertNotIn(excluded, listed)
        # The superseded fresh-checkout simulator is asserted absent too, but
        # by PATTERN rather than by filename: Phase 1's acceptance is a
        # literal grep-zero claim on that name, so spelling it here would
        # make this test the thing that fails the acceptance it supports.
        self.assertEqual([g for g in listed if "fresh_runner" in g], [])


class StaleRosterTest(unittest.TestCase):
    """Six of the seventeen entries are scheduled for deletion by three
    members across two phases. When one of those deletions lands without the
    matching tuple edit, the failure MUST name the phase and owner -- a
    generic `can't open file` sends the reader to look at Docker, which is
    the most expensive wrong turn available here."""

    def test_missing_guard_names_its_phase_and_owner(self):
        with mock.patch.object(
            guards, "GUARDS", (("native/scripts/ci/check_step_order.py",),)
        ):
            tmp = Path(tempfile.mkdtemp(prefix="ceyx-guards-stale."))
            problems = guards._preflight(tmp)
        self.assertEqual(len(problems), 1)
        self.assertIn("STALE GUARDS ENTRY", problems[0])
        self.assertIn("Phase 2", problems[0])
        self.assertIn("impl-p2-render-opus", problems[0])

    def test_unscheduled_missing_guard_says_so_rather_than_inventing_an_owner(self):
        with mock.patch.object(guards, "GUARDS", (("native/scripts/not_a_guard.py",),)):
            tmp = Path(tempfile.mkdtemp(prefix="ceyx-guards-stale."))
            problems = guards._preflight(tmp)
        self.assertEqual(len(problems), 1)
        self.assertIn("NO scheduled retirement", problems[0])

    def test_removed_check_flag_is_diagnosed_not_reported_as_a_guard_failure(self):
        """The Phase 3 shape, and the nastier one: the FILE still exists, so
        the existence preflight passes and the script dies on an
        unrecognised flag while looking perfectly healthy."""
        result = mock.Mock()
        result.stdout = ""
        result.stderr = "usage: gen_linkage_table.py\nerror: unrecognized arguments: --check\n"
        diagnosis = guards._classify_failure(
            ("native/scripts/gen_linkage_table.py", "--check"), result
        )
        self.assertIn("STALE GUARDS ENTRY", diagnosis)
        self.assertIn("no longer accepts --check", diagnosis)
        self.assertIn("Phase 3", diagnosis)

    def test_an_ordinary_guard_failure_is_not_misdiagnosed_as_roster_drift(self):
        """The common case must stay quiet, or a real finding gets buried
        under a bookkeeping message that does not apply."""
        result = mock.Mock()
        result.stdout = "[gen_linkage_table] FAIL -- table does not match manifest\n"
        result.stderr = ""
        self.assertEqual(
            guards._classify_failure(
                ("native/scripts/gen_linkage_table.py", "--check"), result
            ),
            "",
        )

    def test_retirement_schedule_only_names_real_guards(self):
        """The map and the tuple must not drift apart -- a schedule row for a
        path nobody runs is a lie that outlives the guard."""
        listed = {g[0] for g in guards.GUARDS}
        for path in guards.RETIREMENT_SCHEDULE:
            self.assertIn(path, listed, f"{path} is scheduled but not in GUARDS")

    def test_one_entry_is_scheduled_for_retirement(self):
        """13 - 1 (Phase 3) = 12 at the end of the migration.

        Phase 2's five landed, so its rows are gone from the schedule with
        the guards they described. The single remaining row is Phase 3's
        linkage-table entry, and it is the DANGEROUS shape: Phase 3 removes
        that script's `--check` MODE, not the script, so a bare existence
        check would pass while the guard silently stopped working."""
        self.assertEqual(len(guards.RETIREMENT_SCHEDULE), 1)
        phases = [p for p, _ in guards.RETIREMENT_SCHEDULE.values()]
        self.assertEqual(phases.count("Phase 2"), 0)
        self.assertEqual(phases.count("Phase 3"), 1)


class ScopePrintingTest(unittest.TestCase):
    def test_scope_names_both_coverage_and_non_coverage(self):
        """Printed on every run by contract. An unnamed absence is not a
        declared one -- which is the failure this campaign keeps re-finding."""
        buf = io.StringIO()
        with redirect_stdout(buf):
            guards.print_scope()
        out = buf.getvalue()
        self.assertIn(f"count={len(guards.GUARDS)}", out)
        for guard in guards.GUARDS:
            self.assertIn(guard[0], out)
        self.assertEqual(out.count("GUARDS_SCOPE_DOES_NOT_COVER:"), len(guards.NOT_COVERED))
        # The three exclusions a reader is most likely to wrongly assume are
        # covered.
        self.assertIn("libraw", out)
        self.assertIn("selftest", out)
        self.assertIn("macOS", out)


class CostSummaryTest(unittest.TestCase):
    """The reviewer found a hard-coded cost sentence quoting a HOST run's
    figures (21.4s / 2.8s) inside a CONTAINER run's artifact whose own
    GUARDS_SECONDS lines said 49.2s / 8.8s. The summary is now derived from
    the measurements rather than transcribed beside them; these tests pin
    that it stays derived."""

    def test_summary_is_computed_from_the_supplied_durations(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            guards._print_cost_summary([("slow.py", 49.2), ("a.py", 6.0), ("b.py", 2.8)])
        out = buf.getvalue()
        self.assertIn("slowest=slow.py 49.2s", out)
        self.assertIn("other_2_combined=8.8s", out)
        self.assertIn("ratio=5.6x", out)       # 49.2 / 8.8
        self.assertIn("block_total=58.0s", out)

    def test_no_hardcoded_timing_figures_survive_in_the_scope_text(self):
        """The specific regression: a number of the form `N.Ns` baked into
        the scope prose. Scope prints before anything is timed, so any
        second-figure there is necessarily transcribed and will go stale."""
        buf = io.StringIO()
        with redirect_stdout(buf):
            guards.print_scope()
        cost_lines = [ln for ln in buf.getvalue().splitlines() if "GUARDS_SCOPE_COST" in ln]
        self.assertEqual(len(cost_lines), 1)
        self.assertNotRegex(
            cost_lines[0],
            r"\d+\.\d+\s*s",
            "scope cost text must not carry transcribed timings -- it prints "
            "before anything has been measured; put figures in "
            "GUARDS_COST_SUMMARY, which is computed",
        )

    def test_summary_survives_a_zero_second_remainder(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            guards._print_cost_summary([("only.py", 1.0)])
        self.assertIn("n/a", buf.getvalue())


class DispatchRegistrationTest(unittest.TestCase):
    def test_guards_verb_is_registered_with_both_modes(self):
        parser = _load_ci_entry().build_parser()
        args = parser.parse_args(["guards", "--docker"])
        self.assertEqual(args.command, "guards")
        self.assertTrue(args.docker)
        self.assertFalse(args.in_container)
        args = parser.parse_args(["guards", "--in-container"])
        self.assertFalse(args.docker)
        self.assertTrue(args.in_container)
        args = parser.parse_args(["guards"])
        self.assertFalse(args.docker)

    def test_docker_and_in_container_are_mutually_exclusive(self):
        """Asking for both is a confused request, and the confusion should
        surface at the CLI rather than resolve itself silently into one of
        them."""
        parser = _load_ci_entry().build_parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(["guards", "--docker", "--in-container"])


class ShlexRoundTripTest(unittest.TestCase):
    def test_substitution_is_declared_in_band(self):
        """The accepted deviation comes with a condition: a reader of a green
        log must not be able to mistake `<REPO_ROOT>` for a literal argument
        that was passed to docker. Asserted on the real emitter, not on a
        promise in a docstring."""
        buf = io.StringIO()
        with mock.patch.object(guards.run, "run", return_value=mock.Mock(
            returncode=0, stdout="", stderr=""
        )):
            with redirect_stdout(buf):
                guards.run_in_docker(_REPO_ROOT)
        out = buf.getvalue()
        self.assertIn("GUARDS_DOCKER_CMD_IS_SUBSTITUTED=1", out)
        self.assertIn(f"token={guards.REPO_ROOT_TOKEN}", out)
        # and what it stood for on THIS host
        self.assertIn(f"GUARDS_DOCKER_CMD_SUBSTITUTION_VALUE={_REPO_ROOT}", out)
        self.assertIn("is NOT verbatim", out)

    def test_printed_string_parses_back_to_the_argv(self):
        """The printed GUARDS_DOCKER_CMD is the acceptance artifact; it must
        be a faithful, re-runnable rendering of the argv rather than a
        pretty-printed approximation of it."""
        image = guards.read_pinned_image(_REPO_ROOT)
        argv = guards.render_docker_argv(image)
        self.assertEqual(shlex.split(guards.render_docker_cmd(image)), argv)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
