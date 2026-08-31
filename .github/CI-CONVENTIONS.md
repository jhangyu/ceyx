# CI Conventions — ceyx (producer side)

Enforceable rules for every `.github/workflows/*.yml` in this repo. MUST/NEVER
are grep-verifiable; "how to verify" lines are the actual check, not aspiration.

## 1. One workflow per role, not per artifact

Allowed roles only:
- `build` — orchestrator (`build.yml`), four explicit platform jobs + publish
  gate. Not a GitHub Actions `matrix:` over platforms — `uses:` for a reusable
  workflow must be a static path, so `${{ matrix.workflow }}` in a `uses:`
  line is a workflow startup error, not a runtime failure (`build.yml:46-55`).
  A `matrix:` MAY still exist *inside* a single platform leg (e.g.
  `macos_build.yml`'s arm64/x86_64 legs).
- `<platform>_build` — reusable, `workflow_call` only (`macos_build.yml`,
  `linux_build.yml`, `windows_build.yml`, `android_build.yml`).
- `<component>_dist_<platform>` — reusable + `workflow_dispatch`, produces a
  committed third-party binary tree (`heif_dist_windows.yml`,
  `jxl_dist_windows.yml`, `webp_dist_windows.yml`).
- publish — a `needs:`-gated job inside `build.yml`, not a separate file.

A new workflow file outside these roles MUST carry a header comment
`# RATIONALE: <why this cannot extend an existing workflow>` and be called out
in the PR description.

Verify: `ls .github/workflows/*.yml` — every filename maps to one of the roles
above; any new file without a `# RATIONALE:` header in its first 5 lines is a
violation.

## 2. Naming is a single source of truth

All `upload-artifact` names and release asset stems MUST be
`<component>-<platform>-<arch>[.<ext>]`, vocabulary from
`native/deps/arch_map.toml`. `x86_64` uses an underscore; `x86-64` is NEVER
valid in an artifact/asset name. The `upload-artifact` name and the release
asset stem MUST be the same string — no rename step between build and
publish.

Exception: third-party target-triple vocabulary that is not itself an
artifact/asset name is exempt when annotated. Example:
`macos_build.yml:110-117` sets `X86_AOT_TARGET: x86-64-osx-...` because that
is the literal Halide-accepted triple string, not a name this repo mints; the
adjacent comment block records the exception. New exceptions of this kind
MUST carry the same kind of comment at the point of use.

Verify: `grep -rn 'x86-64' .github/workflows/ | grep -v '# \|AOT_TARGET'` →
zero hits outside annotated third-party-triple exceptions. Check
`upload-artifact` `name:` lines and release asset filenames specifically, not
every occurrence of the string in comments/prose.
`grep -rn 'apple-silicon\|intel' .github/workflows/` → zero hits in
`upload-artifact`/asset-name contexts (human-facing `label:` fields exempt).

## 3. Release publishing goes through one module

Only `native/scripts/publish_release.py` (argv-driven via
`--manifest`/`--artifacts-dir`, `--tag`, `--repo`, `--staging-dir`, no
hardcoded tag) may
create or upload release assets. No workflow step other than the single
publish job in `build.yml` may call `gh release upload` or
`softprops/action-gh-release`. Scratch drivers (`native/scripts/tmp/**`) MUST
NEVER be the producer of a shipped release.

Verify: `grep -rln 'gh release upload\|softprops/action-gh-release'
.github/workflows/` → only the publish job's file, and only once.

## 4. `artifacts.lock` is mandatory

Every release run MUST generate `artifacts.lock` covering every uploaded
asset, and MUST execute the download-back verification
(`verify_release_assets`) in the same run before the job is considered green.
An asset produced but absent from the lock is a release defect, not a nit.

Verify: `python3 native/scripts/publish_release.py --help` exits 0; the
publish job log contains both a lock-generation step and a
download-back-verify step.

## 5. Atomic groups stay atomic

Assets that must be installed together travel and are verified as one group.
The canonical example: the Windows trio (`dng_decoder_native.dll` +
`heif.dll` + `libde265.dll`) now ships bundled *inside a single archive*,
`dng_decoder_native-windows-x86_64.tar.gz` (`windows_build.yml:709-714`),
rather than as three separate flat assets — bundling into one archive is
itself the atomicity mechanism, so there is nothing for a downstream consumer
to partially fetch. A count assertion in the packaging step MUST still fail
the run if any DLL is missing from that archive before it is uploaded.

Verify: `windows_build.yml` still hard-fails unless the expected DLL count is
staged into the trio archive (grep for the staging-count check); the archive
name matches the canonical `<component>-<platform>-<arch>.tar.gz` scheme.

## 6. Downstream consumption is hash-pinned, never "latest"

This repo does not control halcyon's pin, but MUST NOT make "latest"-style
consumption the only path: every published asset carries a sha256 in
`artifacts.lock`, and the publish job's download-back step re-verifies those
hashes against the just-uploaded bytes before declaring success.

Verify: `artifacts.lock` from a run contains a `sha256` field per asset entry.

## 7. Triggers must match intent

A gate meant to protect `main` MUST include `pull_request: [main]` (or be a
job inside `build.yml`, which runs on PR by construction).
`push: ci/**`-only triggers are for bootstrapping a new workflow file and MUST
be paired with a real trigger (PR/push-main or `workflow_call`) before merge.

Verify: `grep -A3 '^on:' <workflow>.yml` for each file — no workflow ships
long-term with only a `ci/**` push trigger.

## 8. Committed binary inputs carry provenance

Any tracked third-party binary tree (e.g. `native/third_party/*-dist-windows`)
MUST have a `PROVENANCE.md` naming the producing workflow, upstream versions,
and hashes. Deleting the producing workflow requires first proving the tree
can be regenerated another way — this is why the three `*_dist_windows.yml`
workflows are kept, not deleted, in this round.

Verify: `find native/third_party -maxdepth 1 -name PROVENANCE.md`.

## 9. Deletion requires a consumer audit, not a run count

GitHub Actions run history expires; a zero `gh run list` count proves
retention, not non-use. Any proposal to delete a workflow MUST show (a) its
trigger conditions and (b) a repo-wide grep for consumers of its produced
artifacts — not just an empty run list.

Verify: PR description for any workflow deletion links both checks.

## 10. Workflow-lint (deferred)

A mechanical `workflow-lint` job enforcing rules 1, 2, and 7 on every PR
touching `.github/workflows/` is planned but not implemented this round
(tracked as a follow-up; see `docs/logs/2026-08-31/ci-integration-proposals.md`
§6, §7 Plan C). Do not assume it runs today.
