"""Unit tests for native/scripts/ci/capability.py (WI-16a).

`_probe.main` is mocked throughout (no real dlopen, no real built library
required) except the two dlopen-failure-path tests, which reuse the real
script against a nonexistent path -- the same "instrument itself could not
run" error surface `native/scripts/tests/test_codec_capability_probe.py`
already covers, exercised here through capability.py's wrapper instead.

Android is NOT a capability-vector leg (pinned below, and by a repo-wide
grep asserting no workflow calls this module's CLI for android) -- tested
first because it is the sharpest AC in this WI, not an afterthought.
"""

from __future__ import annotations

import io
import os
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import capability as cap

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
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main, \
             mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("LD_LIBRARY_PATH", None)
            rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
            self.assertEqual(rc, 0)
            self.assertIn("native/third_party/heif-dist-linux/lib",
                          os.environ.get("LD_LIBRARY_PATH", ""))
            self.assertIn("CODEC_CAPABILITY_PROBE_RC=0", out)
        fake_main.assert_called_once()

    def test_windows_sets_no_library_path_var(self):
        # Windows relies on DLL colocation, not an env var (module docstring
        # divergence 2) -- pinning its ABSENCE the same way codec_probe.py's
        # test_macos_run_env_has_no_library_path_var pins macOS's absence.
        before = os.environ.get("LD_LIBRARY_PATH")
        with mock.patch.object(cap._probe, "main", return_value=0):
            cap.capability_vector("windows", "build", expect_cap=["ICC=0"])
        after = os.environ.get("LD_LIBRARY_PATH")
        self.assertEqual(before, after)

    def test_macos_uses_dylib_path_not_targets_artifact(self):
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main:
            cap.capability_vector(
                "macos", "codec", dylib_path="/tmp/fake.dylib", expect=["jpeg:encode=1"]
            )
        argv = fake_main.call_args[0][0]
        self.assertEqual(argv[0], "/tmp/fake.dylib")

    def test_json_out_parent_dir_created(self):
        import tempfile
        with mock.patch.object(cap._probe, "main", return_value=0):
            with tempfile.TemporaryDirectory() as tmp:
                tmp_json = str(Path(tmp) / "out" / "capability.json")
                cap.capability_vector(
                    "windows", "codec", expect=["jpeg:encode=1"], json_out=tmp_json
                )
                self.assertTrue(Path(tmp_json).parent.is_dir())

    def test_json_out_flag_forwarded_to_probe_argv(self):
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main:
            cap.capability_vector(
                "windows", "codec", expect=["jpeg:encode=1"],
                json_out="native/scripts/deps/probe/capability.json",
            )
        argv = fake_main.call_args[0][0]
        self.assertIn("--json-out", argv)
        self.assertEqual(argv[argv.index("--json-out") + 1],
                         "native/scripts/deps/probe/capability.json")

    def test_linux_has_no_json_out_flag_when_not_given(self):
        with mock.patch.object(cap._probe, "main", return_value=0) as fake_main:
            cap.capability_vector("linux", "codec", expect=["jpeg:encode=1"])
        argv = fake_main.call_args[0][0]
        self.assertNotIn("--json-out", argv)

    def test_rc_marker_name_codec(self):
        with mock.patch.object(cap._probe, "main", return_value=0):
            _rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
        self.assertIn("CODEC_CAPABILITY_PROBE_RC=0", out)

    def test_rc_marker_name_build(self):
        with mock.patch.object(cap._probe, "main", return_value=0):
            _rc, out, _err = _emit(
                cap.capability_vector, "linux", "build", expect_cap=["ICC=0"]
            )
        self.assertIn("BUILD_CAPABILITY_PROBE_RC=0", out)

    def test_nonzero_rc_propagates(self):
        with mock.patch.object(cap._probe, "main", return_value=1):
            rc, out, _err = _emit(
                cap.capability_vector, "linux", "codec", expect=["jpeg:encode=1"]
            )
        self.assertEqual(rc, 1)
        self.assertIn("CODEC_CAPABILITY_PROBE_RC=1", out)


class DlopenFailureTests(unittest.TestCase):
    """Real (unmocked) codec_capability_probe.main() against a nonexistent
    library -- the instrument-could-not-run path, same shape
    native/scripts/tests/test_codec_capability_probe.py already covers for
    the underlying script, exercised here through the wrapper."""

    def test_missing_library_exits_nonzero(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            rc, _out, err = _emit(
                cap.capability_vector, "linux", "codec",
                expect=["jpeg:encode=1"],
            )
        # targets.spec("linux")["artifact_path"] won't exist in this test
        # environment either, so this exercises the same dlopen-failure path.
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
    """One golden per platform per kind (6 total): linux/macos(native)/
    windows x codec/build. macOS's cross-leg configure-log path has its own
    dedicated tests above (ConfigureLogTests) rather than a golden fixture,
    since it is a different algorithm with different output shape, not a
    variant of the same emission this fixture family captures."""

    def _run(self, platform, kind, **kwargs):
        with mock.patch.object(cap._probe, "main", side_effect=_fake_probe_main_ok):
            return _emit(cap.capability_vector, platform, kind, **kwargs)

    def test_codec_golden_linux(self):
        rc, out, _err = self._run("linux", "codec", expect=_CODEC_EXPECT)
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-codec-linux.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_build_golden_linux(self):
        rc, out, _err = self._run("linux", "build", expect_cap=_BUILD_EXPECT_CAP)
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-build-linux.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_codec_golden_macos(self):
        rc, out, _err = self._run(
            "macos", "codec", dylib_path="/tmp/fake.dylib", expect=_CODEC_EXPECT
        )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-codec-macos.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_build_golden_macos(self):
        rc, out, _err = self._run(
            "macos", "build", dylib_path="/tmp/fake.dylib", expect_cap=_BUILD_EXPECT_CAP
        )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-build-macos.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_codec_golden_windows(self):
        rc, out, _err = self._run("windows", "codec", expect=_CODEC_EXPECT)
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-codec-windows.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_build_golden_windows(self):
        rc, out, _err = self._run("windows", "build", expect_cap=_BUILD_EXPECT_CAP)
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "capability-vector-build-windows.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)


if __name__ == "__main__":
    unittest.main()
