# libjxl distribution (Windows arm64) — provenance

Windows-on-ARM twin of `native/third_party/libjxl-dist-windows/` (x86_64). Same
sources, pins, carrier and flags as that tree — read its PROVENANCE.md for the
full version/hash/flag record and licence notes; only the facts that differ
are recorded here. Committed for the same reason as the x64 tree: no
contributor machine can build Windows binaries, so these bytes are a
reviewed, pinned input changed only by a visible diff (contract
armci-contract.md R-7, windows-arm64 leg 2026-09-30).

- Contents: static libjxl + bundled highway, brotli, skcms (same tag and flags as the x64 dist).
- Built by: `native/scripts/build_deps.py build jxl-stack --platform windows --arch arm64`
  (via `native/scripts/ci.py dist-build`), workflow `.github/workflows/jxl_dist_windows.yml`
  ("libjxl dist (Windows)"), matrix row `arch_tag: arm64`, runner `windows-11-arm`
  (native arm64 build; MSVC environment `amd64_arm64`, compiler clang-cl).
- CI run: https://github.com/jhangyu/ceyx/actions/runs/36892772306 — head
  712e8c669868747f8eb70c36a49cc4bca08ae66e; artifact `libjxl-dist-windows-arm64`, downloaded with `gh run download` and committed
  unmodified except for this file.
- Architecture: every `.dll` / `.lib` object verified `architecture: aarch64`
  (llvm-objdump -f, all members; `file`: "PE32+ executable (DLL) (GUI)
  Aarch64" for the DLLs) — evidence kept in the working tree at
  docs/logs/2026-10-01/winarm/dist-arm64-verify.txt.
- Arch-derived deltas from the x64 build: no x86 SIMD target-feature flags
  (`/clang:-msse4.1`, `/clang:-mavx2`; native/deps/arch_map.toml fields
  `clang_cl_sse41`/`clang_cl_avx2` are empty for arm64 — the upstreams gate
  their own SIMD sources on the target ISA).
- No `.pins` stamp: actions/upload-artifact@v4 excludes hidden files by
  default and the Windows dist workflows do not set `include-hidden-files`,
  so the CI-written stamp did not travel with the artifact. It is not
  reconstructed by hand.
