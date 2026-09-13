#!/usr/bin/env python3
"""Guard (b) — errexit-defeats-RC-capture (WI-51, parking-lot round).

PROPERTY GUARDED (verbatim from the ruling, `tmp/verify/lead10/USER-RULING-G.md`
guard (b) §1): *a step that reports its own exit status actually reports it* --
no step may emit a self-captured RC marker (`echo "SOMETHING_RC=$?"`) that the
shell's failure semantics make UNREACHABLE on the failure path.

THE DEFECT, verbatim. GitHub Actions' default shell for a non-Windows runner
is `bash --noprofile --norc -eo pipefail {0}` -- `-e` (errexit) is ON unless a
step explicitly opts out via a custom `shell:` invocation template. Under
`-e`, if any command in a `run:` body exits non-zero, the shell exits THAT
INSTANT -- a trailing `echo "X_RC=$?"` meant to capture the preceding
command's real exit code is therefore executed ONLY on the success path. On
failure it is skipped entirely and the marker `X_RC=` is simply absent from
the log (not `X_RC=1`, not anything -- absent). A marker that can only ever
be seen carrying `0` is not an RC capture; it is a constant.

THE HISTORICAL RED CASE, byte-captured (NOT live at this repo's tip as of
R2 -- see below): `.github/workflows/macos_build.yml`'s "D6 layer 1" step,
`6229efb0` -- no `shell:` key, `set -u` (which does NOT affect errexit),
then `echo "D6_LAYER1_RC=$?"` as the step's last line, with no `set +e`
anywhere in the body. See
`native/scripts/ci/tests/fixtures/errexit_rc_probe_defect.yml` for the
byte-verbatim capture (matches `tmp/verify/pyci-parking-lot.md`'s
"ORDERED-2's PROBE FIXTURE"). This is this guard's acceptance evidence per
USER-RULING-G.md guard (b) §3 ("the fixture is the guarantee; flagging the
live step is the bonus, not required") -- it is a state of a real commit and
does not expire when the live file is later fixed.

R2 CORRECTION (lead14 review, caught before I mis-verified it live): the
live `macos_build.yml` D6 step has SINCE been fixed independently (now
carries `set +e` -- see below) and ALSO changed shape, from an inline
`echo "X_RC=$?"` to a `X_RC=$?` bare assignment followed by a separate
`echo "X_RC=${X_RC}"` on a later line. The first cut of this guard only
recognized the echo shape, so it silently reported "0 markers found"
against the live file -- a false statement of NON-COVERAGE that read
identically to a true statement of SAFETY. Fixed by recognizing the
assignment shape too (`RC_ASSIGN_RE`) and by printing `MARKERS_EXAMINED=N`
on every run so a 0-found pass can never again be mistaken for an N-checked
pass.

WHAT MAKES A STEP SAFE (either one discharges the finding for that marker):
  * a `set +e` (or `set +o errexit`) line in the step body BEFORE the marker
    (the echo shape, or the `VAR_RC=$?` assignment shape) -- errexit is
    explicitly turned off for the remainder of the step;
  * an explicit custom `shell:` invocation template (contains the literal
    `{0}` GitHub substitutes the script path into) whose flags do not
    include `-e` / `--errexit` -- e.g. `shell: bash --noprofile --norc -o
    pipefail {0}`. A BARE `shell: bash` (no `{0}` template) is NOT a custom
    invocation -- GitHub still applies its own default `-eo pipefail` flags
    to it, so it is exactly as unsafe as declaring no `shell:` key at all.

SCOPE: bash-shelled steps only (the property is specific to POSIX shell
errexit semantics). A step whose `shell:` names something other than a bash
variant (`pwsh`, a bare `python`, ...) is not evaluated -- `$?` does not mean
the same thing there, and this guard does not attempt to reason about it.

Usage: python3 native/scripts/ci/check_errexit_rc_capture.py [FILE ...]
Zero args scans every `.github/workflows/*.yml`; one or more FILE args (used
by tests, and usable ad hoc against a fixture) scans exactly those files
instead -- REPO_ROOT-relative or absolute paths both work.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
NATIVE_DIR = _SCRIPT_DIR.parent.parent
REPO_ROOT = NATIVE_DIR.parent

if not __package__:
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import workflow_scan  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# A self-captured-RC marker, TWO recognized shapes -- both anchored on the
# literal `$?`, which is what makes the marker a *capture* of the immediately
# preceding command's exit status rather than an arbitrary constant string:
#   (a) `echo "SOMETHING_RC=$?"` / `echo 'SOMETHING_RC=$?'` -- the shape named
#       in the ruling and the original D6 fixture (captured at 6229efb0).
#   (b) `SOMETHING_RC=$?` as a bare shell VARIABLE ASSIGNMENT -- the shape the
#       live D6 step actually uses today (`D6_LAYER1_RC=$?` on its own line,
#       echoed via `${D6_LAYER1_RC}` on a LATER line). This is the REAL
#       capture point: if the preceding command's non-zero exit aborts the
#       step under default errexit, THIS line never runs, and no later
#       `echo "...${SOMETHING_RC}"` can rescue it -- so this line, not the
#       echo, is what must be checked for reachability. Caught late: the
#       original (a)-only version of this guard printed "0 markers found"
#       against this exact live step not because the step was safe, but
#       because this shape was invisible to it (lead14 review, WI-51 R1).
RC_ECHO_RE = re.compile(
    r'echo\s+["\']([A-Za-z_][A-Za-z0-9_]*_RC)=\$\?["\']'
)
RC_ASSIGN_RE = re.compile(
    r'^([A-Za-z_][A-Za-z0-9_]*_RC)=\$\?\s*;?\s*$'
)

# Explicitly disables errexit for the rest of the step body from this point on.
SET_PLUS_E_RE = re.compile(r"(?<![\w-])set\s+\+(e\b|o\s+errexit\b)")

_STEP_NAME_RE = re.compile(r"^-\s+name:\s*(.*)$")
_SHELL_KEY_RE = re.compile(r"^shell:\s*(.+)$")
_RUN_KEY_RE = re.compile(r"^run:\s*(\||>[-+]?)?\s*(.*)$")


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def _step_shell_by_run_start_line(text: str) -> dict:
    """Maps a `run:` key's 1-based line number -> that step's `shell:` value
    (`None` if the step has no `shell:` key). A `shell:` key always precedes
    `run:` within a step in this repo's workflows (grep-verified before
    writing this), and the map only needs the nearest one per step, so a
    lightweight independent scan (not `workflow_scan.iter_run_steps`, which
    does not track `shell:` at all) is enough -- this file owns this small
    parser rather than extending the shared, multi-owner `workflow_scan.py`.
    """
    lines = text.splitlines()
    shell_for_current_step = None
    out = {}
    for i, raw in enumerate(lines):
        stripped = raw.lstrip(" ")
        if _STEP_NAME_RE.match(stripped):
            shell_for_current_step = None
            continue
        m_shell = _SHELL_KEY_RE.match(stripped)
        if m_shell:
            shell_for_current_step = _unquote(m_shell.group(1))
            continue
        if _RUN_KEY_RE.match(stripped):
            out[i + 1] = shell_for_current_step
    return out


def _shell_disables_errexit(shell_value):
    """Tri-state: True (errexit OFF, safe), False (errexit ON, unsafe), or
    None (not a bash step -- not applicable, this guard does not evaluate it).
    """
    if shell_value is None:
        return False  # GitHub default: bash --noprofile --norc -eo pipefail {0}
    s = shell_value.strip()
    if "{0}" not in s:
        # A named-only shell (`shell: bash`, `shell: pwsh`, ...) is not a
        # custom invocation template -- GitHub still applies its own default
        # flags for the named shell. Only "bash" carries the errexit property
        # this guard reasons about.
        if not s.startswith("bash"):
            return None
        return False
    if not s.startswith("bash"):
        return None
    flags_part = s.split("{0}", 1)[0]
    if re.search(r"(?<!\S)-e(?!\S)", flags_part) or "errexit" in flags_part:
        return False  # explicit -e / --errexit: still on
    return True


def scan_text(text: str, workflow_rel: str):
    """Returns (violations, safe) -- each a list of
    `(workflow_rel, line_no, step_name, marker, shell_repr, reason, line_text)`.
    `reason` is only present for `safe` entries (why the marker is reachable).
    """
    shell_map = _step_shell_by_run_start_line(text)
    violations = []
    safe = []
    for step in workflow_scan.iter_run_steps(text, workflow_rel):
        shell_value = shell_map.get(step.start_line)
        errexit_off = _shell_disables_errexit(shell_value)
        if errexit_off is None:
            continue  # non-bash step, not applicable
        set_plus_e_seen = False
        for line_no, line_text in step.body_lines:
            stripped = line_text.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if SET_PLUS_E_RE.search(stripped):
                set_plus_e_seen = True
                continue
            m = RC_ECHO_RE.search(stripped) or RC_ASSIGN_RE.match(stripped)
            if not m:
                continue
            marker = m.group(1)
            shell_repr = shell_value if shell_value is not None else "(default bash -e)"
            if errexit_off or set_plus_e_seen:
                reason = "set +e precedes it" if set_plus_e_seen else "custom shell: without -e"
                safe.append(
                    (workflow_rel, line_no, step.step_name, marker, shell_repr, reason, stripped)
                )
            else:
                violations.append(
                    (workflow_rel, line_no, step.step_name, marker, shell_repr, "", stripped)
                )
    return violations, safe


def _resolve_targets(argv):
    if argv:
        out = []
        for a in argv:
            p = Path(a)
            out.append(p if p.is_absolute() else (REPO_ROOT / a))
        return out
    if not WORKFLOWS_DIR.is_dir():
        return []
    return sorted(WORKFLOWS_DIR.glob("*.yml"))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    targets = _resolve_targets(argv)
    if not targets:
        print(f"[FAIL] no workflow file(s) found (dir={WORKFLOWS_DIR}, argv={argv})")
        return 1

    all_violations = []
    all_safe = []
    scanned = 0
    for path in targets:
        if not path.is_file():
            print(f"[FAIL] target not found: {path}")
            return 1
        scanned += 1
        try:
            rel = str(path.relative_to(REPO_ROOT))
        except ValueError:
            rel = str(path)
        text = path.read_text(encoding="utf-8")
        v, s = scan_text(text, rel)
        all_violations.extend(v)
        all_safe.extend(s)

    examined = len(all_safe) + len(all_violations)
    print(f"[check_errexit_rc_capture] scanned {scanned} workflow file(s), "
          f"MARKERS_EXAMINED={examined} (a pass over 0 examined markers is "
          "not the same claim as a pass over N safe ones -- printed on every "
          "run, pass or fail, so a green log cannot hide a zero).")

    if all_safe:
        print(f"[check_errexit_rc_capture] {len(all_safe)} RC-capture marker(s) "
              "confirmed REACHABLE on the failure path:")
        for wf, line_no, step_name, marker, shell_repr, reason, line_text in all_safe:
            print(f"  {wf}:{line_no}: [{marker}] step={step_name!r} shell={shell_repr!r} "
                  f"({reason}) -- {line_text}")
    else:
        print("[check_errexit_rc_capture] 0 RC-capture markers found using an "
              "explicit errexit-off discipline.")

    if all_violations:
        print(f"[FAIL] {len(all_violations)} self-captured-RC marker(s) are "
              "UNREACHABLE on their step's failure path -- the shell's default "
              "errexit (`bash -e`) will exit before the echo runs, so the "
              "marker can only ever emit its success value. Add `set +e` "
              "before the echo, or an explicit `shell: bash ... {0}` template "
              "without `-e`:")
        for wf, line_no, step_name, marker, shell_repr, _reason, line_text in all_violations:
            print(f"  {wf}:{line_no}: [{marker}] step={step_name!r} shell={shell_repr!r} "
                  f"-- {line_text}")
        return 1

    print(f"[check_errexit_rc_capture] PASS -- MARKERS_EXAMINED={examined}, "
          f"{examined} of {examined} confirmed reachable on their step's "
          "failure path (0 violations).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
