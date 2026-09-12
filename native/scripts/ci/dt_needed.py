"""DT_NEEDED import-closure verification for the staged Linux HEIF companions.

Replaces linux_build.yml's "Verify DT_NEEDED references the staged HEIF
companions" step (:983-1011, d33cc607). Two levels, exactly as today:

1. The decoder's own NEEDED entry must name ``libheif.so.1`` directly.
2. The staged ``libheif.so.1``'s own NEEDED entry must name
   ``libde265.so.0`` transitively (the decoder itself has no direct
   DT_NEEDED entry for libde265 -- GNU ld's ``--as-needed`` only records a
   direct dependency for a symbol actually referenced, and no translation
   unit here calls a de265_* symbol directly).

De-pipelined (readelf output to a file, then match the file text in Python)
per the pipefail lesson: ``readelf | grep -q`` inverts on a SUCCESSFUL match
under ``pipefail`` (grep exits first, readelf dies of SIGPIPE, pipeline
reports 141) -- ``run.run_to_file`` plus a Python substring match never hits
that trap (C-G3/run.py rule 2).
"""

from __future__ import annotations

from pathlib import Path

from . import report, run


def _needed_level(so_path: Path, dump_path: Path, needle: str, error_text: str) -> int:
    result = run.run_to_file(["readelf", "-d", str(so_path)], dump_path)
    dump_text = dump_path.read_text(encoding="utf-8", errors="replace")
    report.plain(dump_text.rstrip("\n"))
    if result.returncode != 0:
        rc = result.returncode
    else:
        matched = [line for line in dump_text.splitlines() if needle in line]
        for line in matched:
            report.plain(line)
        rc = 0 if matched else 1
    report.bare_rc(rc, f"DT_NEEDED {needle} in {so_path}")
    if rc != 0:
        report.error(error_text)
        return 1
    return 0


def dt_needed(platform: str, artifact_dir: str, runner_temp: str) -> int:
    """Replaces :983-1011, both levels. Returns 1 if either level fails."""
    native_dir = Path(artifact_dir) / "native"
    so = native_dir / "libdng_decoder_native.so"
    heif_so = native_dir / "libheif.so.1"

    report.section("Level 1: decoder's own NEEDED (expect libheif.so.1)")
    rc = _needed_level(
        so,
        Path(runner_temp) / "dt_needed_decoder.txt",
        "libheif.so.1",
        f"{so} has no DT_NEEDED entry for libheif.so.1 — the staged "
        "companion would never be loaded.",
    )
    if rc != 0:
        return rc

    report.section("Level 2: libheif.so.1's own NEEDED (expect libde265.so.0, transitive)")
    return _needed_level(
        heif_so,
        Path(runner_temp) / "dt_needed_heif.txt",
        "libde265.so.0",
        f"{heif_so} has no DT_NEEDED entry for libde265.so.0 — the HEVC "
        "decoder linkage is UNVERIFIED, refusing to publish.",
    )
