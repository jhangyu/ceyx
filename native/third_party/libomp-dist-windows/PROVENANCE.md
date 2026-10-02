# LLVM OpenMP runtime (libomp) — Windows x86_64 — provenance

Committed dist of the LLVM OpenMP runtime that the Windows decoder links
against (`lib/libomp.lib`) and ships beside itself (`bin/libomp140.x86_64.dll`).
User ruling R-11 (2026-10-02, Halcyon docs/logs/2026-09-30/armci-contract.md):
built from source here because Microsoft ships the LLVM OpenMP runtime only
as debug_nonredist on the arm64 image, and vcomp140 (OpenMP 2.0) cannot run
RawSpeed3's OpenMP 3.0–4.5 constructs. Consumed by native/cmake/heif.cmake
(the only OpenMP source on Windows; no System32 / VC-redist fallback).

| Field | Value |
|---|---|
| Upstream | LLVM OpenMP runtime, https://github.com/llvm/llvm-project (openmp/) |
| Version | `llvmorg-22.1.8` |
| Source | https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/llvm-project-22.1.8.src.tar.xz |
| Source sha256 | `922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888` (GitHub release digest; re-hashed locally) |
| Licence | Apache-2.0 WITH LLVM-exception — `share/licenses/libomp/LICENSE.TXT` (openmp/LICENSE.TXT) |
| Built by | `native/scripts/build_deps.py build libomp-stack --platform windows --arch x86_64` (carrier `native/scripts/deps/win_libomp_dist.py`, via `ci.py dist-build`), workflow `.github/workflows/libomp_dist_windows.yml` |
| CI run | https://github.com/jhangyu/ceyx/actions/runs/36943142762 (job 110639057621), head `8915db2b00bf1b39721d4648e472c91cf25afb75`, `LIBOMP_DIST_WINDOWS_RC=0` |
| Runner | `windows-latest` (image windows-2025-vs2026 20260925.250.1), MSVC env `x64`, VCTools 14.51.36231, compiler clang-cl 20.1.8 |
| Artifact | `libomp-dist-windows-x86_64`, downloaded with `gh run download`, committed unmodified (the carrier's diagnostic `.libomp_*.txt` dumps excluded) plus this file |
| `bin/libomp140.x86_64.dll` sha256 | `5941c2bba2b2b18e65856e859a3dbf394ae49f48459a9ec6e4a01dc3a8f41c43` |
| `lib/libomp.lib` sha256 | `12361c3293ad6c54a317d91c01b2ac20e438c89908d45fd12abbdcec03d58c0b` |

CMake options (`cmake -S openmp -G Ninja`, standalone; only `openmp/` and the
shared top-level `cmake/` are extracted from the tarball):

```
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=clang-cl
  -DCMAKE_CXX_COMPILER=clang-cl
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded   (static CRT)
  -DOPENMP_MSVC_NAME_SCHEME=ON                 (DLL libomp140.<LIBOMP_ARCH>.dll, import lib libomp.lib)
  -DLIBOMP_ARCH=x86_64
```

Post-install: the upstream backwards-compat copies `libiomp5md.dll`/`.lib` are
removed. `.pins` is the carrier's stamp (travelled with the artifact via
`include-hidden-files: true`).

Verified (CI carrier asserts + local re-check, docs/logs/2026-10-02/vcomp/libomp-dist-verify.txt
in the working tree): PE machine `file format coff-x86-64`
(llvm-objdump -f, positive control on the committed heif.dll of each arch);
exports `__kmpc_fork_call`, `__kmpc_for_static_init_4`, `omp_get_num_threads`;
imports only KERNEL32.dll + PSAPI.DLL (no VCRUNTIME140); every `libomp.lib`
member names `libomp140.x86_64.dll`.
