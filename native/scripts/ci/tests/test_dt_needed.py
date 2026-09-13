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


class TestDtNeededAndroid(unittest.TestCase):
    """Android is NOT linux with a parameter swapped -- two independent
    checks (STL bidirectional presence/absence, then device-whitelist
    closure via the existing standalone script), per lead6-pyci-opus's
    WI-22 ruling. See dt_needed.py's module docstring."""

    def _dirs(self):
        d = Path(tempfile.mkdtemp())
        artifact_dir = d / "artifacts"
        (artifact_dir / "native").mkdir(parents=True)
        runner_temp = d / "runnertemp"
        runner_temp.mkdir()
        ndk_home = d / "ndk"
        readelf_dir = ndk_home / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin"
        readelf_dir.mkdir(parents=True)
        llvm_readelf = readelf_dir / "llvm-readelf"
        llvm_readelf.write_text("#!/bin/sh\n")
        llvm_readelf.chmod(0o755)
        build_log = d / "android_build.log"
        return artifact_dir, runner_temp, ndk_home, build_log

    def _stage_decoder(self, artifact_dir):
        so = artifact_dir / "native" / "libdng_decoder_native.so"
        so.write_bytes(b"")
        return so

    def test_stl_shared_with_libcxx_and_closure_ok_passes(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        self._stage_decoder(artifact_dir)
        build_log.write_text(dt_needed._ANDROID_STL_SHARED_LOG_LINE + "\n")
        dumps = {
            "libdng_decoder_native.so": " 0x01 (NEEDED) Shared library: [libc++_shared.so]\n",
        }
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)), \
             mock.patch.object(dt_needed.run, "capture", return_value=(0, "CLOSURE_RESULT=ok\n")):
            rc, out, err = _emit(
                dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
                ndk_home=str(ndk_home), build_log=str(build_log),
            )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("STL_SHARED_LOG=True", out)
        self.assertIn("DT_NEEDED_HAS_LIBCXX_SHARED=True", out)
        self.assertIn("DT_NEEDED_CLOSURE_RC=0", out)

    def test_stl_shared_but_libcxx_missing_fails(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        self._stage_decoder(artifact_dir)
        build_log.write_text(dt_needed._ANDROID_STL_SHARED_LOG_LINE + "\n")
        dumps = {"libdng_decoder_native.so": " 0x01 (NEEDED) Shared library: [libc.so]\n"}
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(
                dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
                ndk_home=str(ndk_home), build_log=str(build_log),
            )
        self.assertEqual(rc, 1)
        self.assertIn("does not list libc++_shared.so", err)

    def test_stl_static_with_libcxx_present_fails(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        self._stage_decoder(artifact_dir)
        build_log.write_text(dt_needed._ANDROID_STL_STATIC_LOG_LINE + "\n")
        dumps = {"libdng_decoder_native.so": " 0x01 (NEEDED) Shared library: [libc++_shared.so]\n"}
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(
                dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
                ndk_home=str(ndk_home), build_log=str(build_log),
            )
        self.assertEqual(rc, 1)
        self.assertIn("lists libc++_shared.so, but the configure log says the STL is c++_static", err)

    def test_stl_static_without_libcxx_then_closure_fail_propagates(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        self._stage_decoder(artifact_dir)
        build_log.write_text(dt_needed._ANDROID_STL_STATIC_LOG_LINE + "\n")
        dumps = {"libdng_decoder_native.so": " 0x01 (NEEDED) Shared library: [libc.so]\n"}
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)), \
             mock.patch.object(dt_needed.run, "capture", return_value=(1, "CLOSURE_RESULT=fail:x.so:liby.so\n")):
            rc, out, err = _emit(
                dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
                ndk_home=str(ndk_home), build_log=str(build_log),
            )
        self.assertEqual(rc, 1)
        self.assertIn("DT_NEEDED_CLOSURE_RC=1", out)
        self.assertIn("neither bundled nor in the measured", err)

    def test_neither_stl_log_line_present_fails(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        self._stage_decoder(artifact_dir)
        build_log.write_text("some unrelated configure output\n")
        dumps = {"libdng_decoder_native.so": " 0x01 (NEEDED) Shared library: [libc++_shared.so]\n"}
        with mock.patch.object(dt_needed.run, "run_to_file", side_effect=_fake_run_to_file(dumps)):
            rc, out, err = _emit(
                dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
                ndk_home=str(ndk_home), build_log=str(build_log),
            )
        self.assertEqual(rc, 1)
        self.assertIn("cannot be identified", err)

    def test_missing_ndk_home_fails(self) -> None:
        artifact_dir, runner_temp, _ndk_home, build_log = self._dirs()
        rc, out, err = _emit(
            dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
            ndk_home=None, build_log=str(build_log),
        )
        self.assertEqual(rc, 2)
        self.assertIn("--ndk-home is required", err)

    def test_llvm_readelf_not_executable_fails(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        # Point at an NDK root that has no llvm-readelf staged at all.
        empty_ndk = ndk_home.parent / "empty_ndk"
        rc, out, err = _emit(
            dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
            ndk_home=str(empty_ndk), build_log=str(build_log),
        )
        self.assertEqual(rc, 1)
        self.assertIn("llvm-readelf not found", err)

    def test_no_decoder_so_staged_fails(self) -> None:
        artifact_dir, runner_temp, ndk_home, build_log = self._dirs()
        rc, out, err = _emit(
            dt_needed.dt_needed, "android", str(artifact_dir), str(runner_temp),
            ndk_home=str(ndk_home), build_log=str(build_log),
        )
        self.assertEqual(rc, 1)
        self.assertIn("no libdng_decoder_native", err)


class TestDtNeededUnsupportedPlatform(unittest.TestCase):
    def test_windows_is_not_silently_accepted(self) -> None:
        d = Path(tempfile.mkdtemp())
        rc, out, err = _emit(dt_needed.dt_needed, "windows", str(d), str(d))
        self.assertEqual(rc, 2)
        self.assertIn("unsupported platform 'windows'", err)


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
