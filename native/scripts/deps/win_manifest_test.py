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

    A ``source.windows`` block qualifies. A ``source.default`` of
    ``kind = "registry"`` ALSO qualifies as of D1-a (2026-08-31,
    spec-windows-codec-full-green.md): `native/vcpkg/triplets/x64-windows-heif.cmake`
    exists and names aom as a static vcpkg port, so vcpkg's registry leg now
    covers Windows too -- it is no longer macOS/Linux-only, which is what made
    a registry-resolved component unacquirable on Windows before this ruling.
    Any OTHER `source.default` kind (tarball/git) still needs a concrete URL
    or ref and is treated as acquirable outright, same as before.
    """
    source = component_block.get("source", {})
    if "windows" in source:
        return True
    default = source.get("default", {})
    return bool(default) and default.get("kind") in ("registry", "tarball", "git")


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
        """Demonstrated red: switching on a codec whose component has no
        Windows acquisition at all (no ``source.windows`` and no
        ``source.default``) must be caught. Synthesised rather than reusing
        `aom` because D1-a (2026-08-31) gave `aom` a real Windows acquisition
        path (its registry `source.default`, now vcpkg-resolvable on Windows
        via `x64-windows-heif.cmake`) -- reusing it here would no longer
        demonstrate a violation, it would demonstrate the fix."""
        switches = dict(_CODEC_SWITCH_TO_COMPONENT)
        switches["WITH_UNACQUIRABLE_TEST_CODEC"] = "_unacquirable_test_component"
        flags = dict(self.flags)
        flags["WITH_UNACQUIRABLE_TEST_CODEC"] = "ON"
        components = dict(self.components)
        components["_unacquirable_test_component"] = {"source": {}}
        unsatisfiable = [
            f"{switch}=ON but {component} has no Windows acquisition"
            for switch, component in switches.items()
            if flags.get(switch) == "ON" and not _has_windows_acquisition(components[component])
        ]
        self.assertEqual(len(unsatisfiable), 1, unsatisfiable)
        self.assertIn("_unacquirable_test_component", unsatisfiable[0])


class TestWindowsDistIsFullCapability(unittest.TestCase):
    """Un-parked 2026-08-31 by docs/logs/2026-08-31/spec-windows-codec-full-green.md
    (in-scope item 1), which is the product-scope ruling this class (formerly
    ``TestWindowsDistIsDecodeOnly``, pinning the lead's OPTION 1 ruling) is
    superseded by rather than kept alongside -- WITH_KVAZAAR=OFF and
    WITH_KVAZAAR=ON cannot both pass."""

    def setUp(self) -> None:
        self.flags = _rendered(manifest.load())

    def test_kvazaar_and_aom_are_on(self) -> None:
        self.assertEqual(self.flags.get("WITH_KVAZAAR"), "ON")
        self.assertEqual(self.flags.get("WITH_AOM_DECODER"), "ON")
        self.assertEqual(self.flags.get("WITH_AOM_ENCODER"), "ON")

    def test_hevc_decode_stays_on(self) -> None:
        """Decode must not decay into decode-nothing."""
        self.assertEqual(self.flags.get("WITH_LIBDE265"), "ON")

    def test_gpl_encoder_stays_off(self) -> None:
        """x265 is GPL-2.0 and is excluded by name, independently of the
        encode-capability ruling."""
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


class TestWindowsLibheifFullCapabilityFlags(unittest.TestCase):
    """Windows must render the same encoder flag set as macOS/Linux.

    Un-parked by docs/logs/2026-08-31/spec-windows-codec-full-green.md
    (in-scope item 1). Before that ruling this rendered WITH_KVAZAAR=OFF and
    WITH_AOM_*=OFF.

    Uses the module's actual API (``render.render`` / manual source-block
    inspection) rather than the plan's illustrative ``manifest.render_cmake_args``
    / ``manifest.resolve_source`` names, which this tree does not export
    (grep -n '^def ' native/scripts/deps/manifest.py); the assertions
    themselves are identical to the plan's.
    """

    def test_windows_libheif_renders_full_capability_flags(self) -> None:
        loaded = manifest.load()
        argv = render.render(loaded, "libheif", "windows", "x86_64", dist="/D")
        joined = " ".join(argv)
        self.assertIn("-DWITH_KVAZAAR=ON", joined)
        self.assertIn("-DWITH_AOM_DECODER=ON", joined)
        self.assertIn("-DWITH_AOM_ENCODER=ON", joined)
        # Licence red line, asserted in the SAME test so it cannot be relaxed
        # by accident while flipping the three above.
        self.assertIn("-DWITH_X265=OFF", joined)

    def test_windows_aom_has_a_resolvable_source(self) -> None:
        """[component.aom] must be acquirable on Windows.

        Before this change its source.default was registry-only with a
        comment stating no Windows acquisition existed, which made the
        libheif Windows configure unsatisfiable the moment WITH_AOM_*
        flipped ON.
        """
        loaded = manifest.load()
        aom = loaded["manifest"]["component"]["aom"]
        self.assertTrue(_has_windows_acquisition(aom))
        src = aom["source"].get("windows") or aom["source"]["default"]
        self.assertIn(src["kind"], ("registry", "tarball", "git"))


if __name__ == "__main__":
    unittest.main()
