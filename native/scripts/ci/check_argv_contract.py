#!/usr/bin/env python3
"""Guard (g) -- REGISTERED-COMMAND-COUNT CHECK, one piece of the argv
contract property.

STATED PROPERTY OF (g) (full scope, `pyci-ruling-G-guard-plan.md` "(g) argv
contract check"): every `ci.py` invocation a workflow can actually execute
is accepted by the parser that will receive it, including after matrix
substitution.

>>> SCOPE -- READ THIS BEFORE BELIEVING A PASS <<<
THIS FILE IMPLEMENTS ONLY THE COMMAND-PATH-COUNT SLICE OF (g), NOT THE FULL
PROPERTY. It walks the REAL `argparse` parser object returned by
`ci.py`'s own `build_parser()` -- never a regex/text scan of source, and
never a re-implementation of how `_add_platform_command` or the nested
`provision`/`vcpkg` subparsers register themselves -- and asserts that the
set of registered command PATHS (a "path" is a tuple of subcommand names
reachable by walking every `_SubParsersAction.choices` at every depth, e.g.
`("provision", "ninja")` or `("min-runtime",)`) exactly matches a frozen set
of 36 entries. It does NOT replay workflow YAML through the parser and does
NOT validate matrix-substituted argv strings -- that half of (g) is a
SEPARATE, NOT-YET-BUILT piece (see `pyci-ruling-G-guard-plan.md` (g)§3:
"Do not accept a green from a text-matching implementation"). A reader of a
clean run of THIS script must not conclude the full (g) property holds.

WHY 36, NOT 35: the ruling in
`tmp/verify/lead16/SIGNOFF-LEDGER.md` ("Inherited rulings I am holding
unchanged") states `ci.py` has 36 registered command paths, not 35 --
planner6/planner7's "35 registered, 0 unregistered invocations" figure
(`pyci-ruling-G-guard-plan.md:503`, `parking-lot-round-decomposition.md:202`)
counted WORKFLOW INVOCATIONS matched against registered top-level commands,
not the full set of registered parser nodes. Counted independently here by
walking the parser tree: 28 top-level `add_parser()` calls in
`build_parser()` (2 flat + 13 via `_add_platform_command` + 13 more flat,
including `provision` and `vcpkg` themselves as invocable leaves with no
subcommand) PLUS 8 nested leaves (`provision`'s 3: `ninja`/`apt`/
`locate-clang-cl`; `vcpkg`'s 5: `baseline`/`bootstrap`/`install`/
`assert-aom-artifact`/`export-prefix`) = 36. `test_dispatch.py`'s existing
`FROZEN_CLI_SURFACE` test covers only the 28 top-level names and does not
descend into `provision`/`vcpkg`'s children -- this guard is not a
duplicate of that test, it is a strict superset at a different tree depth.

WHY THE REAL PARSER OBJECT, NOT A REGEX: `pyci-ruling-G-guard-plan.md`
(g)§2 records a verified red for exactly this shortcut -- a prototype
regex over `add_parser("...")` text reported 13 phantom "unregistered"
subcommands because `ci.py` also registers many of them through the
`_add_platform_command` helper driven by the `_PLATFORM_COMMANDS`/
`_PLATFORMLESS_COMMANDS` tuples. "The guard must obtain the parser object,
not re-implement knowledge of how parsers are built." This file imports
`ci.py`'s own `build_parser()` and walks the live object for exactly that
reason.

Run with:
  python3 native/scripts/ci/check_argv_contract.py
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

if not __package__:
    import ci.report as report  # noqa: E402
else:
    from . import report  # noqa: E402

_CI_PY_PATH = REPO_ROOT / "native" / "scripts" / "ci.py"

# Frozen expected surface, ruled at 36 (see module docstring). Each entry is
# a full command PATH, deepest-first not required -- comparison is a plain
# set-equality so both an addition and a removal are reported by name.
EXPECTED_COMMAND_PATHS: frozenset[tuple[str, ...]] = frozenset(
    {
        ("selftest",),
        ("marker-diff",),
        ("verify-artifact",),
        ("import-closure",),
        ("min-runtime",),
        ("assert-exports",),
        ("assert-no-avx512",),
        ("assert-orientation",),
        ("codec-probe",),
        ("capability-vector",),
        ("assert-staged-companions",),
        ("stage",),
        ("assert-staged-group",),
        ("dt-needed",),
        ("assert-vcpkg-artefacts",),
        ("assert-configure-log",),
        ("vcpkg-baseline",),
        ("vcpkg-bootstrap",),
        ("vcpkg-install",),
        ("verify-interpreter",),
        ("ensure-cmake",),
        ("build-zlib",),
        ("locate-clang-cl",),
        ("verify-vulkan-lib",),
        ("dist-build",),
        ("dist-list",),
        ("provision",),
        ("provision", "ninja"),
        ("provision", "apt"),
        ("provision", "locate-clang-cl"),
        ("vcpkg",),
        ("vcpkg", "baseline"),
        ("vcpkg", "bootstrap"),
        ("vcpkg", "install"),
        ("vcpkg", "assert-aom-artifact"),
        ("vcpkg", "export-prefix"),
    }
)

EXPECTED_COMMAND_COUNT = 36
assert len(EXPECTED_COMMAND_PATHS) == EXPECTED_COMMAND_COUNT, (
    f"EXPECTED_COMMAND_PATHS itself has {len(EXPECTED_COMMAND_PATHS)} entries, "
    f"not {EXPECTED_COMMAND_COUNT} -- this is a bug in this file, fix the set "
    "literal above before trusting anything this guard reports."
)


def load_ci_entrypoint():
    """Loads `native/scripts/ci.py` directly from its file path under a
    distinct module name -- NOT `import ci`, which resolves to the
    same-named `ci/` PACKAGE directory (the path finder prefers a package
    over a same-named module), the same Q5 shape `test_dispatch.py` already
    documents and works around."""
    spec = importlib.util.spec_from_file_location(
        "ci_entrypoint_under_argv_contract", _CI_PY_PATH
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _subparsers_action(parser: argparse.ArgumentParser):
    if parser._subparsers is None:
        return None
    for action in parser._subparsers._group_actions:
        if hasattr(action, "choices"):
            return action
    return None


def enumerate_command_paths(parser: argparse.ArgumentParser) -> set[tuple[str, ...]]:
    """Walks the REAL parser tree recursively and returns every registered
    command path -- one entry per `add_parser()` call reachable from the
    root, at every depth. A parent that itself carries subparsers (e.g.
    `provision`) is included as its own leaf path IN ADDITION TO its
    children, because `ci.py provision` with no subcommand is itself a
    parseable, dispatchable invocation (`provision_command` is `None`, not
    a parse error)."""
    paths: set[tuple[str, ...]] = set()

    def _walk(node: argparse.ArgumentParser, prefix: tuple[str, ...]) -> None:
        action = _subparsers_action(node)
        if action is None:
            return
        for name, child in action.choices.items():
            child_path = prefix + (name,)
            paths.add(child_path)
            _walk(child, child_path)

    _walk(parser, ())
    return paths


def main(argv=None) -> int:
    print(
        "ARGV_CONTRACT_SCOPE=command-path-count-only: this guard does NOT replay "
        "workflow YAML through the parser and does NOT validate matrix-substituted "
        "argv -- see module docstring. A pass here is not a pass of the full (g) "
        "property."
    )

    ci_entrypoint = load_ci_entrypoint()
    parser = ci_entrypoint.build_parser()
    actual_paths = enumerate_command_paths(parser)

    added = sorted(actual_paths - EXPECTED_COMMAND_PATHS)
    removed = sorted(EXPECTED_COMMAND_PATHS - actual_paths)

    print(f"ARGV_CONTRACT_EXPECTED_COUNT={EXPECTED_COMMAND_COUNT}")
    print(f"ARGV_CONTRACT_ACTUAL_COUNT={len(actual_paths)}")

    for path in added:
        report.error(
            f"[argv-contract] ci.py registers {' '.join(path)!r} which is NOT in "
            "the frozen 36-command-path surface. If this addition is intended, "
            "update EXPECTED_COMMAND_PATHS in "
            "native/scripts/ci/check_argv_contract.py in the SAME COMMIT."
        )
    for path in removed:
        report.error(
            f"[argv-contract] ci.py no longer registers {' '.join(path)!r}, which "
            "the frozen 36-command-path surface expects. If this removal is "
            "intended, update EXPECTED_COMMAND_PATHS in "
            "native/scripts/ci/check_argv_contract.py in the SAME COMMIT."
        )

    if added or removed:
        print("ARGV_CONTRACT_RESULT=FAIL")
        return 1
    print("ARGV_CONTRACT_RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
