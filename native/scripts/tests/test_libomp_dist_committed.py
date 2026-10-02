"""R-11 (2026-10-02): the Windows OpenMP runtime comes from the committed
LLVM dist only. Guards the two halves that must agree -- the declaration
(shipped_files.toml's libomp140.<arch>.dll companion) and the committed trees
heif.cmake links/stages from -- and that heif.cmake has no runner-image
fallback (System32 / VC redist) left to reintroduce."""
from __future__ import annotations

import sys
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(NATIVE / "scripts"))

import read_shipped_files as rsf  # noqa: E402

# canonical arch -> CEYX_WINDOWS_DIST_SUFFIX (cmake/heif.cmake)
_SUFFIX = {"x86_64": "windows", "arm64": "windows-arm64"}


def test_every_declared_omp_companion_exists_in_its_committed_dist():
    windows = rsf.load_declaration()["windows"]
    for arch, suffix in _SUFFIX.items():
        omp = [c for c in rsf.companions_for(windows, arch) if c.startswith("libomp140.")]
        assert len(omp) == 1, (arch, omp)
        dist = NATIVE / "third_party" / f"libomp-dist-{suffix}"
        assert (dist / "bin" / omp[0]).is_file(), dist / "bin" / omp[0]
        assert (dist / "lib" / "libomp.lib").is_file()
        assert f"dll={omp[0]}" in (dist / ".pins").read_text(encoding="utf-8")


def test_heif_cmake_sources_openmp_from_the_dist_only():
    lines = (NATIVE / "cmake" / "heif.cmake").read_text(encoding="utf-8").splitlines()
    text = "\n".join(l for l in lines if not l.lstrip().startswith("#"))  # code only
    assert 'libomp-dist-${CEYX_WINDOWS_DIST_SUFFIX}' in text
    assert "set(OpenMP_libomp_LIBRARY" in text
    for gone in ("OpenMP.LLVM", "System32", "find_file(CEYX_WIN_OMP_RUNTIME", "VCToolsRedistDir"):
        assert gone not in text, gone
