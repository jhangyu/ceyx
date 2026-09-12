"""Tests for native/scripts/ci/workflow_scan.py (WI-4)."""
from __future__ import annotations

import unittest

# Relative import, not `import ci.workflow_scan`: this package is importable
# under two module identities (`ci.*` and `native.scripts.ci.*`, see
# check_shell_prohibition.py's import comment / tmp/verify/pyci-push1-gate-
# VERDICT.md); a relative import is identity-agnostic.
from .. import workflow_scan


INLINE_YAML = """\
jobs:
  build:
    steps:
      - name: Checkout
        uses: actions/checkout@v4
      - name: Run one-liner
        run: python3 native/scripts/ci.py foo
"""

BLOCK_PIPE_YAML = """\
jobs:
  build:
    steps:
      - name: Multi-line block
        run: |
          echo "one"
          echo "two"
"""

FOLDED_YAML = """\
jobs:
  build:
    steps:
      - name: Folded block
        run: >
          echo "folded"
"""

BLANK_LINE_IN_BODY_YAML = """\
jobs:
  build:
    steps:
      - name: Has a blank line
        run: |
          echo "one"

          echo "two"
      - name: Next step
        run: python3 x.py
"""

DEDENT_TERMINATES_YAML = """\
jobs:
  build:
    steps:
      - name: First
        run: |
          echo "body line"
        shell: bash
      - name: Second
        run: python3 y.py
"""

SAME_INDENT_NOT_SWALLOWED_YAML = """\
jobs:
  build:
    steps:
      - name: First
        run: |
          echo "body"
        env:
          FOO: bar
"""

MULTIPLE_STEPS_YAML = """\
jobs:
  build:
    steps:
      - name: Alpha
        run: |
          echo "alpha body"
      - name: Beta
        run: |
          echo "beta body"
      - name: Gamma
        uses: actions/checkout@v4
      - name: Delta
        run: python3 z.py
"""


class TestInlineAndBlock(unittest.TestCase):
    def test_inline_body(self):
        steps = list(workflow_scan.iter_run_steps(INLINE_YAML, "x.yml"))
        self.assertEqual(len(steps), 1)
        step = steps[0]
        self.assertEqual(step.step_name, "Run one-liner")
        self.assertEqual(workflow_scan.code_lines(step), ["python3 native/scripts/ci.py foo"])

    def test_block_pipe_body(self):
        steps = list(workflow_scan.iter_run_steps(BLOCK_PIPE_YAML, "x.yml"))
        self.assertEqual(len(steps), 1)
        self.assertEqual(workflow_scan.code_lines(steps[0]), ['echo "one"', 'echo "two"'])

    def test_folded_block_body(self):
        steps = list(workflow_scan.iter_run_steps(FOLDED_YAML, "x.yml"))
        self.assertEqual(len(steps), 1)
        self.assertEqual(workflow_scan.code_lines(steps[0]), ['echo "folded"'])


class TestBlankAndDedent(unittest.TestCase):
    def test_blank_line_inside_body_is_not_a_separate_step(self):
        steps = list(workflow_scan.iter_run_steps(BLANK_LINE_IN_BODY_YAML, "x.yml"))
        self.assertEqual(len(steps), 2)
        first = steps[0]
        self.assertEqual(first.step_name, "Has a blank line")
        # code_lines strips the blank line, leaving exactly the two echoes.
        self.assertEqual(workflow_scan.code_lines(first), ['echo "one"', 'echo "two"'])

    def test_dedent_terminates_the_body(self):
        steps = list(workflow_scan.iter_run_steps(DEDENT_TERMINATES_YAML, "x.yml"))
        self.assertEqual(len(steps), 2)
        first = steps[0]
        self.assertEqual(workflow_scan.code_lines(first), ['echo "body line"'])
        # "shell: bash" at the dedented indent must NOT appear in the body.
        for _line_no, text in first.body_lines:
            self.assertNotIn("shell:", text)

    def test_body_at_same_indent_as_run_key_is_not_swallowed(self):
        steps = list(workflow_scan.iter_run_steps(SAME_INDENT_NOT_SWALLOWED_YAML, "x.yml"))
        self.assertEqual(len(steps), 1)
        step = steps[0]
        self.assertEqual(workflow_scan.code_lines(step), ['echo "body"'])
        for _line_no, text in step.body_lines:
            self.assertNotIn("env:", text)
            self.assertNotIn("FOO:", text)


class TestStepNameAttribution(unittest.TestCase):
    def test_step_name_attribution_across_multiple_steps(self):
        steps = list(workflow_scan.iter_run_steps(MULTIPLE_STEPS_YAML, "x.yml"))
        # Gamma has no `run:` key (uses: only), so only 3 RunSteps are yielded.
        names = [s.step_name for s in steps]
        self.assertEqual(names, ["Alpha", "Beta", "Delta"])


class TestCodeLines(unittest.TestCase):
    def test_comments_and_blanks_are_removed(self):
        yaml_text = """\
jobs:
  build:
    steps:
      - name: With comments
        run: |
          # a comment
          echo "real"

          # another comment
"""
        steps = list(workflow_scan.iter_run_steps(yaml_text, "x.yml"))
        self.assertEqual(workflow_scan.code_lines(steps[0]), ['echo "real"'])


if __name__ == "__main__":
    unittest.main()
