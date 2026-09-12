"""Unit tests for native/scripts/ci/capability.py (WI-16a).

macOS/windows: `_probe.main` is mocked (in-process call, no real dlopen).
Linux: `run.run` is mocked (subprocess call, see module docstring
divergence-2 correction -- an in-process `LD_LIBRARY_PATH` mutation cannot
affect a dlopen already made by this same process, proven by a docker
reproduction before the fix; linux therefore launches
`codec_capability_probe.py` as a fresh process with an explicit `env`, the
one mechanism that actually works). The two dlopen-failure-path tests reuse
the REAL script (real subprocess on linux, real in-process call otherwise)
against a nonexistent path -- the same "instrument itself could not run"
error surface `native/scripts/tests/test_codec_capability_probe.py` already
covers, exercised here through capability.py's wrapper.

Android is NOT a capability-vector leg (pinned below, and by a repo-wide
grep asserting no workflow calls this module's CLI for android) -- tested
first because it is the sharpest AC in this WI, not an afterthought.
"""

from __future__ import annotations

import io
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import capability as cap
from .. import run as run_module

_REPO_ROOT = Path(__file__).resolve().parents[4]
_GOLDEN_DIR = Path(__file__).resolve().parent / "golden" / "expected"

_CODEC_EXPECT = [
    "jpeg:encode=1", "jpeg:decode=0",
    "webp:encode=1", "webp:decode=1",
    "heic:encode=1", "heic:decode=1",
    "avif:encode=1", "avif:decode=1",
    "jxl:encode=1", "jxl:decode=1",
]
_BUILD_EXPECT_CAP = [
    "ICC=0", "OPENMP=1", "HEIF=1", "WEBP=1", "JXL=1", "RAW=1",
]


def _emit(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


def _fake_probe_main_ok(argv):
    """Reproduces codec_capability_probe.main()'s own OK-path print format
    verbatim (status:9s padding, want==got, trailing summary line) without
    dlopen'ing anything real."""
    n = 0
    i = 0
    while i < len(argv):
        if argv[i] == "--expect":
            fmt, rest = argv[i + 1].split(":", 1)
            direction, want = rest.split("=", 1)
            print(f"{'OK':9s} {fmt}:{direction} want={want} got={want}")
            n += 1
        elif argv[i] == "--expect-cap":
            name, want = argv[i + 1].split("=", 1)
            print(f"{'OK':9s} capability:{name} want={want} got={want}")
            n += 1
        i += 1
    print(f"all {n} capability expectations hold")
    return 0


def _fake_probe_stdout_ok(argv):
    """Same OK-path text as _fake_probe_main_ok, but as a captured string
    (what a subprocess's stdout would contain) rather than printed live --
    used for linux's run.run() mock, since linux invokes the probe as a
    subprocess argv list `[sys.executable, str(_PROBE_SCRIPT), *real_argv]`."""
    lines = []
    n = 0
    i = 0
    while i < len(argv):
        if argv[i] == "--expect":
            fmt, rest = argv[i + 1].split(":", 1)
            direction, want = rest.split("=", 1)
            lines.append(f"{'OK':9s} {fmt}:{direction} want={want} got={want}")
            n += 1
        elif argv[i] == "--expect-cap":
            name, want = argv[i + 1].split("=", 1)
            lines.append(f"{'OK':9s} capability:{name} want={want} got={want}")
            n += 1
        i += 1
    lines.append(f"all {n} capability expectations hold")
    return "\n".join(lines) + "\n"


def _fake_run_ok(argv, cwd=None, env=None):
    return run_module.RunResult(argv=list(argv), returncode=0, stdout=_fake_probe_stdout_ok(argv))


class AndroidNegativeSpaceTests(unittest.TestCase):
    def test_android_rejected_for_codec(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("android", "codec")
        self.assertIn("android is not a capability-vector leg", str(ctx.exception))

    def test_android_rejected_for_build(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("android", "build")
        self.assertIn("android is not a capability-vector leg", str(ctx.exception))

    def test_android_rejected_regardless_of_source(self):
        with self.assertRaises(ValueError):
            cap.capability_vector("android", "codec", source="configure-log")

    def test_no_android_workflow_calls_capability_vector(self):
        """The negative-space AC from WI-16's plan text: no workflow may
        call this module's CLI for android. True today (pre-WI-17/18
        migration, since no YAML calls `ci.py capability-vector` at all
        yet) and must remain true after migration."""
        text = (_REPO_ROOT / ".github" / "workflows" / "android_build.yml").read_text(
            encoding="utf-8"
        )
        self.assertEqual(text.count("ci.py capability-vector"), 0)

    def test_android_skip_line_not_referenced_by_this_module(self):
        """This module must never mention android's SKIP echo -- it belongs
        to another file and this module has no reason to know its text."""
        source = Path(cap.__file__).read_text(encoding="utf-8")
        self.assertNotIn("CAPABILITY_PROBE SKIP", source)


class ValidationTests(unittest.TestCase):
    def test_unknown_kind_rejected(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("linux", "bogus")
        self.assertIn("unknown kind", str(ctx.exception))

    def test_unknown_source_rejected(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("linux", "codec", source="bogus")
        self.assertIn("unknown source", str(ctx.exception))

    def test_macos_probe_requires_dylib_path(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("macos", "codec", source="probe")
        self.assertIn("requires dylib_path", str(ctx.exception))

    def test_linux_rejects_dylib_path(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("linux", "codec", dylib_path="some/path")
        self.assertIn("dylib_path is not accepted for platform='linux'", str(ctx.exception))

    def test_windows_rejects_dylib_path(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("windows", "codec", dylib_path="some/path")
        self.assertIn("dylib_path is not accepted for platform='windows'", str(ctx.exception))

    def test_configure_log_only_valid_for_macos(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector("linux", "codec", source="configure-log")
        self.assertIn("only valid for platform='macos'", str(ctx.exception))

    def test_configure_log_rejects_dylib_path(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector(
                "macos", "codec", source="configure-log", dylib_path="x"
            )
        self.assertIn("dylib_path is not accepted with source='configure-log'", str(ctx.exception))

    def test_configure_log_rejects_json_out(self):
        with self.assertRaises(ValueError) as ctx:
            cap.capability_vector(
                "macos", "codec", source="configure-log", json_out="x.json"
            )
        self.assertIn("json_out is not accepted with source='configure-log'", str(ctx.exception))


class ProbePlumbingTests(unittest.TestCase):
    """Tests the wrapper's own behaviour (env, argv construction, RC
    marker, json-out directory creation) with `_probe.main` mocked."""

    def test_linux_sets_ld_library_path(self):
        """Linux passes LD_LIBRARY_PATH via the SUBPROCESS's env= dict (not
        an os.environ mutation -- see module docstring divergence-2
        correction), so this asserts the env dict `run.run()` was actually
        called with, not the parent process's own os.environ."""
        with mock.patch.object(run_module, "run", side_effect=_fake_run_ok) as fake_run:
            rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
        self.assertEqual(rc, 0)
        self.assertIn("CODEC_CAPABILITY_PROBE_RC=0", out)
        fake_run.assert_called_once()
        env = fake_run.call_args.kwargs["env"]
        self.assertIn("native/third_party/heif-dist-linux/lib", env.get("LD_LIBRARY_PATH", ""))

    def test_windows_sets_no_library_path_var(self):
        # Windows relies on DLL colocation, not an env var (module docstring
        # divergence 2) -- pinning its ABSENCE the same way codec_probe.py's
        # test_macos_run_env_has_no_library_path_var pins macOS's absence.
        before = os.environ.get("LD_LIBRARY_PATH")
        with mock.patch.object(cap._probe, "main", return_value=0):
            _emit(cap.capability_vector, "windows", "build", expect_cap=["ICC=0"])
        after = os.environ.get("LD_LIBRARY_PATH")
        self.assertEqual(before, after)

    def test_macos_uses_dylib_path_not_targets_artifact(self):
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main:
            _emit(
                cap.capability_vector,
                "macos", "codec", dylib_path="/tmp/fake.dylib", expect=["jpeg:encode=1"]
            )
        argv = fake_main.call_args[0][0]
        self.assertEqual(argv[0], "/tmp/fake.dylib")

    def test_json_out_parent_dir_created(self):
        import tempfile
        with mock.patch.object(cap._probe, "main", return_value=0):
            with tempfile.TemporaryDirectory() as tmp:
                tmp_json = str(Path(tmp) / "out" / "capability.json")
                _emit(
                    cap.capability_vector,
                    "windows", "codec", expect=["jpeg:encode=1"], json_out=tmp_json
                )
                self.assertTrue(Path(tmp_json).parent.is_dir())

    def test_json_out_flag_forwarded_to_probe_argv(self):
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main:
            _emit(
                cap.capability_vector,
                "windows", "codec", expect=["jpeg:encode=1"],
                json_out="native/scripts/deps/probe/capability.json",
            )
        argv = fake_main.call_args[0][0]
        self.assertIn("--json-out", argv)
        self.assertEqual(argv[argv.index("--json-out") + 1],
                         "native/scripts/deps/probe/capability.json")

    def test_linux_has_no_json_out_flag_when_not_given(self):
        with mock.patch.object(run_module, "run", side_effect=_fake_run_ok) as fake_run:
            _emit(cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"])
        argv = fake_run.call_args[0][0]
        self.assertNotIn("--json-out", argv)

    def test_linux_argv_is_python_plus_probe_script_plus_real_args(self):
        with mock.patch.object(run_module, "run", side_effect=_fake_run_ok) as fake_run:
            _emit(cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"])
        argv = fake_run.call_args[0][0]
        self.assertEqual(argv[0], sys.executable)
        self.assertTrue(argv[1].endswith("codec_capability_probe.py"))
        self.assertIn("--expect", argv)

    def test_rc_marker_name_codec(self):
        with mock.patch.object(run_module, "run", side_effect=_fake_run_ok):
            _rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
        self.assertIn("CODEC_CAPABILITY_PROBE_RC=0", out)

    def test_rc_marker_name_build(self):
        with mock.patch.object(run_module, "run", side_effect=_fake_run_ok):
            _rc, out, _err = _emit(
                cap.capability_vector, "linux", "build", expect_cap=["ICC=0"]
            )
        self.assertIn("BUILD_CAPABILITY_PROBE_RC=0", out)

    def test_nonzero_rc_propagates(self):
        def _fake_run_fail(argv, cwd=None, env=None):
            return run_module.RunResult(argv=list(argv), returncode=1, stdout="", stderr="boom")

        with mock.patch.object(run_module, "run", side_effect=_fake_run_fail):
            rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
        self.assertEqual(rc, 1)
        self.assertIn("CODEC_CAPABILITY_PROBE_RC=1", out)


class DlopenFailureTests(unittest.TestCase):
    """Real (unmocked) codec_capability_probe.py against a nonexistent
    library -- the instrument-could-not-run path, same shape
    native/scripts/tests/test_codec_capability_probe.py already covers for
    the underlying script. Linux exercises this through a REAL subprocess
    (capability.py's actual mechanism, not a fake), which also incidentally
    proves the subprocess plumbing itself works end-to-end, not just its
    argv construction."""

    def test_missing_library_exits_nonzero_linux_real_subprocess(self):
        rc, _out, err = _emit(
            cap.capability_vector, "linux", "codec",
            expect=["jpeg:encode=1"],
        )
        # targets.spec("linux")["artifact_path"] won't exist in this test
        # environment either, so this exercises the same dlopen-failure path,
        # for real, via the actual subprocess capability.py spawns.
        self.assertEqual(rc, 1)
        self.assertIn("dlopen failed", err)

    def test_missing_library_exits_nonzero_macos(self):
        rc, _out, err = _emit(
            cap.capability_vector, "macos", "codec",
            dylib_path="/nonexistent/lib.dylib", expect=["jpeg:encode=1"],
        )
        self.assertEqual(rc, 1)
        self.assertIn("dlopen failed", err)


class ConfigureLogTests(unittest.TestCase):
    def _tmp_log(self, tmp_path, text):
        log = tmp_path / "cross_stage2_build.log"
        log.write_text(text, encoding="utf-8")
        return log

    def test_codec_variant_matches_literal(self, ):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self._tmp_log(Path(tmp), "-- some line\n-- JXL: static\n-- more\n")
            rc, out, _err = _emit(
                cap.capability_vector, "macos", "codec",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT cross-leg JXL enabled RC=0", out)

    def test_codec_variant_missing_literal_fails(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self._tmp_log(Path(tmp), "-- JXL: shared\n")
            rc, out, err = _emit(
                cap.capability_vector, "macos", "codec",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT cross-leg JXL enabled RC=1", out)
        self.assertIn("may not have enabled JXL", err)

    def test_build_variant_matches_literal(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self._tmp_log(Path(tmp), "[ceyx] LCMS2: disabled (OQ-N4 option Z)\n")
            rc, out, _err = _emit(
                cap.capability_vector, "macos", "build",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT cross-leg ICC=0 RC=0", out)

    def test_build_variant_missing_literal_fails(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self._tmp_log(Path(tmp), "nothing relevant\n")
            rc, out, err = _emit(
                cap.capability_vector, "macos", "build",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT cross-leg ICC=0 RC=1", out)
        self.assertIn("ENABLE_LCMS may not be forced OFF", err)

    def test_missing_log_file_fails_closed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            rc, out, err = _emit(
                cap.capability_vector, "macos", "codec",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 1)
        self.assertIn("RC=1", out)


class GoldenEmissionTests(unittest.TestCase):
    """One golden per distinct emission SHAPE (leader ruling 2026-09-13,
    extending WI-10/R1's precedent to this WI): linux / macos-native /
    windows x codec/build via source="probe" (6), PLUS macos-cross x
    codec/build via source="configure-log" (2) -- eight total. The cross
    leg is a genuinely different algorithm with a disjoint marker
    vocabulary (no CODEC_CAPABILITY_PROBE_RC/BUILD_CAPABILITY_PROBE_RC at
    all), so it gets its own goldens rather than relying on the named
    literal-match unit tests alone: a golden pins the WHOLE emission
    (content, count, order), which the individual `assertIn` tests above
    cannot -- a dropped/duplicated/reordered line would still pass every
    one of them."""

    def _run(self, platform, kind, **kwargs):
        if platform == "linux":
            with mock.patch.object(run_module, "run", side_effect=_fake_run_ok):
                return _emit(cap.capability_vector, platform, kind, **kwargs)
        with mock.patch.object(cap._probe, "main", side_effect=_fake_probe_main_ok):
            return _emit(cap.capability_vector, platform, kind, **kwargs)

    def _golden(self, name):
        path = _GOLDEN_DIR / f"{name}.markers"
        text = path.read_text(encoding="utf-8")
        self.assertGreater(len(text), 0, f"{path} must be non-zero bytes")
        self.assertGreater(len(text.splitlines()), 0, f"{path} must be non-zero lines")
        return text

    def test_codec_golden_linux(self):
        rc, out, _err = self._run("linux", "codec", expect=_CODEC_EXPECT)
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-codec-linux"))

    def test_build_golden_linux(self):
        rc, out, _err = self._run("linux", "build", expect_cap=_BUILD_EXPECT_CAP)
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-build-linux"))

    def test_codec_golden_macos_native(self):
        rc, out, _err = self._run(
            "macos", "codec", dylib_path="/tmp/fake.dylib", expect=_CODEC_EXPECT
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-codec-macos-native"))

    def test_build_golden_macos_native(self):
        rc, out, _err = self._run(
            "macos", "build", dylib_path="/tmp/fake.dylib", expect_cap=_BUILD_EXPECT_CAP
        )
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-build-macos-native"))

    def test_codec_golden_windows(self):
        rc, out, _err = self._run("windows", "codec", expect=_CODEC_EXPECT)
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-codec-windows"))

    def test_build_golden_windows(self):
        rc, out, _err = self._run("windows", "build", expect_cap=_BUILD_EXPECT_CAP)
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-build-windows"))

    def test_codec_golden_macos_cross(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "cross_stage2_build.log").write_text(
                "-- JXL: static\n", encoding="utf-8"
            )
            rc, out, _err = _emit(
                cap.capability_vector, "macos", "codec",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-codec-macos-cross"))

    def test_build_golden_macos_cross(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "cross_stage2_build.log").write_text(
                "[ceyx] LCMS2: disabled (OQ-N4 option Z)\n", encoding="utf-8"
            )
            rc, out, _err = _emit(
                cap.capability_vector, "macos", "build",
                source="configure-log", workspace=tmp,
            )
        self.assertEqual(rc, 0)
        self.assertEqual(out, self._golden("capability-vector-build-macos-cross"))


if __name__ == "__main__":
    unittest.main()
