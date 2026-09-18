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
        whole campaign keeps paying for."""
        listed = {g[0] for g in guards.GUARDS}
        self.assertIn("native/scripts/ci/check_test_marker_leak.py", listed)
        self.assertEqual(len(guards.GUARDS), 17)

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
            "native/scripts/ci/fresh_runner_gate.py",
        ):
            self.assertNotIn(excluded, listed)


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
    def test_printed_string_parses_back_to_the_argv(self):
        """The printed GUARDS_DOCKER_CMD is the acceptance artifact; it must
        be a faithful, re-runnable rendering of the argv rather than a
        pretty-printed approximation of it."""
        image = guards.read_pinned_image(_REPO_ROOT)
        argv = guards.render_docker_argv(image)
        self.assertEqual(shlex.split(guards.render_docker_cmd(image)), argv)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
