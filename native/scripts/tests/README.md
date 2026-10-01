# native/scripts/tests

Unit tests for the CI scripts in `native/scripts/`. Run them manually:

    python3 -m pytest native/scripts/tests/

This suite is deliberately NOT run in CI. CI is compile-only by user ruling
(2026-10-02); `native/scripts/ci/check_no_test_execution_in_ci.py` enforces
that, and its exemption list must not be widened to admit this suite.
