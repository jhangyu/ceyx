"""Windows manifest invariants (R4 Task #8).

The golden file for libheif/windows pins the exact rendered argv, but a golden
is a *transcript*: when someone edits the manifest and regenerates it, the
golden agrees with whatever the manifest now says, including a defect. These
tests encode the two things that must hold REGARDLESS of what the golden
currently contains.

Background: the Windows libheif overlay was originally transcribed from
win_build_round6.sh (the full-capability script on ci/ci-green-round6) rather
than from build_heif_dist_windows.sh @ ci/round3, the decode-only script this
branch actually ships. The render therefore asked for a kvazaar HEVC encoder
and an aom AV1 codec that this branch never builds -- and, worse, that on
Windows it *cannot* build, since [component.aom] has no `source.windows` and
its comment forbids inventing one. The configure was unsatisfiable as frozen.

``test_every_enabled_windows_codec_is_acquirable`` below is deliberately the
GENERAL form of that defect rather than a check for those three flag values:
it fails for any future codec switched ON for Windows without a corresponding
Windows acquisition, which is the actual bug class. The specific decode-only
assertion is kept alongside it to pin the lead's OPTION 1 ruling.

Named ``win_manifest_test.py`` (pytest discovers ``*_test.py``) to stay inside
the ``deps/win_*`` file-ownership boundary for this round -- the shared
``tests/test_manifest.py`` belongs to another owner.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import manifest, render  # noqa: E402

# libheif's WITH_<SWITCH> -> the manifest component that must supply it.
# LIBDE265 is included as the positive control: it IS on for Windows, and it
# DOES have a source.windows block, so it must pass the same invariant that
# kvazaar/aom would have failed.
_CODEC_SWITCH_TO_COMPONENT = {
    "WITH_LIBDE265": "libde265",
    "WITH_KVAZAAR": "kvazaar",
    "WITH_AOM_DECODER": "aom",
    "WITH_AOM_ENCODER": "aom",
}


def _rendered(loaded, component: str = "libheif") -> dict[str, str]:
    argv = render.render(loaded, component, "windows", "x86_64")
    flags: dict[str, str] = {}
    for entry in argv:
        if entry.startswith("-D") and "=" in entry:
            key, _, value = entry[2:].partition("=")
            flags[key] = value
    return flags


def _has_windows_acquisition(component_block: dict) -> bool:
    """True when the component can actually be OBTAINED on Windows.

    A ``source.windows`` block qualifies. A ``source.default`` qualifies ONLY
    if it is not registry-supplied, because the registry (vcpkg) legs of this
    project are macOS/Linux -- an `aom` resolved to `kind = "registry"` has no
    Windows acquisition path in this tree, which is precisely the trap.
    """
    source = component_block.get("source", {})
    if "windows" in source:
        return True
    default = source.get("default", {})
    return bool(default) and default.get("kind") != "registry"


class TestWindowsCodecAcquirability(unittest.TestCase):
    def setUp(self) -> None:
        self.loaded = manifest.load()
        self.flags = _rendered(self.loaded)
        self.components = self.loaded["manifest"]["component"]

    def test_every_enabled_windows_codec_is_acquirable(self) -> None:
        """The general invariant: nothing may be switched ON for Windows that
        Windows has no way to acquire."""
        unsatisfiable = []
        for switch, component in _CODEC_SWITCH_TO_COMPONENT.items():
            if self.flags.get(switch) != "ON":
                continue
            if not _has_windows_acquisition(self.components[component]):
                unsatisfiable.append(f"{switch}=ON but {component} has no Windows acquisition")
        self.assertEqual(unsatisfiable, [], "; ".join(unsatisfiable))

    def test_positive_control_libde265_is_on_and_acquirable(self) -> None:
        """Guards against the invariant passing vacuously: if the switch
        vocabulary above ever stops matching the real flag names, every
        lookup returns None, nothing is ON, and the test would pass while
        checking nothing. libde265 IS on for Windows, so this must hold."""
        self.assertEqual(self.flags.get("WITH_LIBDE265"), "ON")
        self.assertTrue(_has_windows_acquisition(self.components["libde265"]))
        self.assertIn("windows", self.components["libde265"]["source"])

    def test_the_invariant_actually_fires_when_violated(self) -> None:
        """Demonstrated red: flipping aom back ON without giving it a Windows
        acquisition must be caught. This is the exact pre-fix state."""
        self.flags["WITH_AOM_DECODER"] = "ON"
        unsatisfiable = [
            f"{switch}=ON but {component} has no Windows acquisition"
            for switch, component in _CODEC_SWITCH_TO_COMPONENT.items()
            if self.flags.get(switch) == "ON" and not _has_windows_acquisition(self.components[component])
        ]
        self.assertEqual(len(unsatisfiable), 1, unsatisfiable)
        self.assertIn("aom", unsatisfiable[0])


class TestWindowsDistIsDecodeOnly(unittest.TestCase):
    """Pins the lead's OPTION 1 ruling (2026-08-31): the Windows dist
    reproduces build_heif_dist_windows.sh @ ci/round3 -- libde265 + libheif,
    decode-only. The round6 full-capability shape stays parked; un-parking it
    is a product scope change for the user, not a transcription choice."""

    def setUp(self) -> None:
        self.flags = _rendered(manifest.load())

    def test_kvazaar_and_aom_are_off(self) -> None:
        self.assertEqual(self.flags.get("WITH_KVAZAAR"), "OFF")
        self.assertEqual(self.flags.get("WITH_AOM_DECODER"), "OFF")
        self.assertEqual(self.flags.get("WITH_AOM_ENCODER"), "OFF")

    def test_hevc_decode_stays_on(self) -> None:
        """Decode-only must not decay into decode-nothing."""
        self.assertEqual(self.flags.get("WITH_LIBDE265"), "ON")

    def test_gpl_encoder_stays_off(self) -> None:
        """x265 is GPL-2.0 and is excluded by name, independently of the
        decode-only ruling."""
        self.assertEqual(self.flags.get("WITH_X265"), "OFF")

    def test_msvc_defaults_are_restored_by_hand(self) -> None:
        """K13/K14/N16: CMAKE_CXX_FLAGS REPLACES CMake's MSVC defaults rather
        than appending, so losing -EHsc/-GR silently disables exceptions and
        RTTI with no build error -- only a much later runtime failure."""
        cxx = self.flags.get("CMAKE_CXX_FLAGS", "")
        self.assertIn("-EHsc", cxx)
        self.assertIn("-GR", cxx)

    def test_static_crt_is_requested(self) -> None:
        self.assertEqual(self.flags.get("CMAKE_MSVC_RUNTIME_LIBRARY"), "MultiThreaded")


if __name__ == "__main__":
    unittest.main()
