"""Unit tests for native/scripts/ci/dt_needed.py (WI-8)."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # native/scripts/

from ci import dt_needed, markerdiff, run  # noqa: E402

_GOLDEN_DIR = Path(__file__).resolve().parent / "golden" / "expected"
_REPO_ROOT = Path(__file__).resolve().parents[4]
_CI_ENTRYPOINT = _REPO_ROOT / "native" / "scripts" / "ci.py"


def _emit(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


def _fake_run_to_file(dumps):
    """Returns a ``run.run_to_file``-shaped stand-in that writes canned
    readelf output for each ``so`` path, keyed by basename, instead of
    shelling out to a real ``readelf`` (not guaranteed present/ELF-capable
    on every dev host)."""

    def _fn(argv, out_path, cwd=None, env=None):
        so_name = Path(argv[-1]).name
        text = dumps[so_name]
        Path(out_path).write_text(text, encoding="utf-8")
        return run.RunResult(argv=argv, returncode=0, stdout=text, stderr="")

    return _fn


class TestDtNeeded(unittest.TestCase):
    def _dirs(self):
        d = Path(tempfile.mkdtemp())
        artifact_dir = d / "artifacts"
        (artifact_dir / "native").mkdir(parents=True)
        runner_temp = d / "runnertemp"
        runner_temp.mkdir()
        return artifact_dir, runner_temp

    def test_both_present_passes(self) -> None:
        artifact_dir, runner_temp = self._dirs()
        dumps = {
            "libdng_decoder_native.so": " 0x0000000000000001 (NEEDED) Shared library: [libheif.so.1]\n",
            "libheif.so.1": " 0x0000000000000001 (NEEDED) Shared library: [libde265.so.0]\n",
        }
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(dt_needed.dt_needed, "linux", str(artifact_dir), str(runner_temp))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("RC=0 (DT_NEEDED libheif.so.1 in", out)
        self.assertIn("RC=0 (DT_NEEDED libde265.so.0 in", out)

    def test_level1_missing_fails(self) -> None:
        artifact_dir, runner_temp = self._dirs()
        dumps = {
            "libdng_decoder_native.so": " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n",
            "libheif.so.1": " 0x0000000000000001 (NEEDED) Shared library: [libde265.so.0]\n",
        }
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(dt_needed.dt_needed, "linux", str(artifact_dir), str(runner_temp))
        self.assertEqual(rc, 1)
        self.assertIn("RC=1 (DT_NEEDED libheif.so.1 in", out)
        self.assertIn(
            "::error::",
            err,
        )
        self.assertIn("has no DT_NEEDED entry for libheif.so.1", err)

    def test_level2_missing_fails(self) -> None:
        artifact_dir, runner_temp = self._dirs()
        dumps = {
            "libdng_decoder_native.so": " 0x0000000000000001 (NEEDED) Shared library: [libheif.so.1]\n",
            "libheif.so.1": " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n",
        }
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(dt_needed.dt_needed, "linux", str(artifact_dir), str(runner_temp))
        self.assertEqual(rc, 1)
        self.assertIn("RC=0 (DT_NEEDED libheif.so.1 in", out)
        self.assertIn("RC=1 (DT_NEEDED libde265.so.0 in", out)
        self.assertIn("has no DT_NEEDED entry for libde265.so.0", err)

    def test_emission_matches_golden(self) -> None:
        artifact_dir, runner_temp = self._dirs()
        dumps = {
            "libdng_decoder_native.so": " 0x0000000000000001 (NEEDED) Shared library: [libheif.so.1]\n",
            "libheif.so.1": " 0x0000000000000001 (NEEDED) Shared library: [libde265.so.0]\n",
        }
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, _ = _emit(dt_needed.dt_needed, "linux", str(artifact_dir), str(runner_temp))
        self.assertEqual(rc, 0)
        emitted = markerdiff.extract(out)
        golden = (_GOLDEN_DIR / "dt-needed-linux.markers").read_text(encoding="utf-8")
        self.assertEqual(emitted, golden.splitlines())


class TestDtNeededBareScriptInvocation(unittest.TestCase):
    """Owed item (leader ruling on WI-8 signoff): a genuine subprocess
    invocation of ``python3 native/scripts/ci.py dt-needed ...`` -- the
    exact call shape linux_build.yml:1087 uses -- not an in-process
    ``ci_entrypoint.main()`` call. See test_stage.py's identical class for
    the full rationale (not duplicated here). Routed through ``run.run()``,
    not a raw ``subprocess.run()``: `check_no_test_execution_in_ci.py`'s
    AST scan flags any bare `subprocess.*` call under
    `native/scripts/ci/**.py`, test files included, as
    `[subprocess-outside-run]`."""

    def _run_ci(self, *argv: str):
        return run.run([sys.executable, str(_CI_ENTRYPOINT), *argv], cwd=str(_REPO_ROOT))

    def test_dt_needed_bare_script_missing_so_is_handled_not_traceback(self) -> None:
        # No decoder .so staged: readelf against a missing path is a
        # legitimate real-world red case (a prior stage step failed) and
        # must surface as a handled RC=1 through the real CLI, never an
        # unhandled Python traceback -- this is what "exercises the real
        # call shape" is actually checking: that argparse wiring + argument
        # plumbing + the readelf subprocess call all cooperate end to end.
        d = Path(tempfile.mkdtemp())
        artifact_dir = d / "artifacts"
        (artifact_dir / "native").mkdir(parents=True)
        runner_temp = d / "runnertemp"
        runner_temp.mkdir()

        result = self._run_ci(
            "dt-needed", "--platform", "linux",
            "--artifact-dir", str(artifact_dir),
            "--runner-temp", str(runner_temp),
        )
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("DT_NEEDED", result.stdout)
        self.assertIn(
            "has no DT_NEEDED entry for libheif.so.1", result.stderr
        )

    def test_dt_needed_bare_script_missing_required_flag_is_argparse_error(self) -> None:
        # No --runner-temp at all: argparse must reject this before any
        # gate logic runs, exit 2, not a traceback from a missing attribute.
        result = self._run_ci(
            "dt-needed", "--platform", "linux", "--artifact-dir", "/tmp/does-not-matter"
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
