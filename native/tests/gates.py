"""How every CMake test executable in native/cmake/tests.cmake is run.

kind = "runner:<file>"   -- invoked by that local runner (the gate path)
       "ci-build:<file>" -- built (never run) by CI; <file> references it
       "manual:<usage>"  -- manual tool; one-line usage. Kept per the
                            2026-10-02 orphan verdicts (design-B-debt.md §2),
                            each compile-proven once (plan-B-debt.md Task 20).
check_test_manifest.py fails when an executable is missing here.
"""

GATES: dict[str, str] = {
    "test_decode": "runner:native/tests/run_decode_matrix.py",
    "dng_ffi_harness": "runner:native/tests/run_decode_matrix.py",
    "test_device_handoff": "runner:native/tests/run_decode_matrix.py",
    "test_cfa_phase": "runner:native/tests/run_decode_matrix.py",
    "test_cfa_color": "runner:native/tests/run_decode_matrix.py",
    "test_sized_decode": "runner:native/tests/run_decode_matrix.py",
    "test_stage4_oriented": "runner:native/tests/run_decode_matrix.py",
    "test_abi_layout": "runner:native/tests/run_decode_matrix.py",
    "ceyx_encode_harness": "runner:native/tests/run_decode_matrix.py",
    "test_decode_android": "runner:native/tests/run_decode_matrix.py",
    "dng_ffi_harness_android": "runner:native/tests/run_decode_matrix.py",
    "test_device_handoff_android": "runner:native/tests/run_decode_matrix.py",
    "test_android_vulkan_capability": "runner:native/tests/run_decode_matrix.py",
    "test_raw_contract_abi": "runner:native/tests/run_raw_matrix.py",
    "test_raw_layout_contract": "runner:native/tests/run_raw_matrix.py",
    "test_raw_file_router": "runner:native/tests/run_raw_matrix.py",
    "test_libraw_frontend": "runner:native/tests/run_raw_matrix.py",
    "test_libraw_adapter": "runner:native/tests/run_raw_matrix.py",
    "test_raw_render_params": "runner:native/tests/run_raw_matrix.py",
    "test_raw_auto_exposure": "runner:native/tests/run_raw_matrix.py",
    "test_raw_bayer_kernel": "runner:native/tests/run_raw_matrix.py",
    "test_raw_xtrans_kernel": "runner:native/tests/run_raw_matrix.py",
    "test_raw_linear_rgb_kernel": "runner:native/tests/run_raw_matrix.py",
    "test_raw_end_to_end": "runner:native/tests/run_raw_matrix.py",
    "test_raw_hardening": "runner:native/tests/run_raw_matrix.py",
    "test_raw_sized_decode": "runner:native/tests/run_raw_matrix.py",
    "raw_corpus_hash_baseline": "runner:native/tests/run_nikon_he_gate.py",
    "raw_corpus_ev_gate": "runner:native/tests/run_nikon_he_gate.py",
    "libraw_smoke": "runner:native/tests/run_nikon_he_gate.py",
    "test_codec_heif": "runner:native/tests/run_dist_equivalence.py",
    "test_concurrent_decode": "runner:native/tests/run_parallel_bench.py",
    "probe_concurrent_raw": "runner:native/tests/run_parallel_bench.py",
    "orient_capability_probe": "ci-build:native/scripts/ci/orientation.py",
    "test_errmap_dst_too_small": "manual:test_errmap_dst_too_small <bayer.dng> <bayer.arw> <xtrans.raf> <x3f>",
    "test_yuv420_to_rgba": "manual:test_yuv420_to_rgba  (mem8 v3 T13 ceyx_yuv420_to_rgba8 suite C1..C5; no args)",
    "test_stage4_yuv420_output": "manual:test_stage4_yuv420_output  (mem8 v3 T12 yuv420 output arm Y1..Y9; see header)",
    "test_ceyx_decode_into": "manual:test_ceyx_decode_into <bayer.dng> <lossy.dng> <bayer.arw>",
    "test_ceyx_orient": "manual:test_ceyx_orient  (native-rotation Task 1 orient reference check; no args)",
    "test_raw_diagnostics_freshness": "manual:test_raw_diagnostics_freshness <generic-raw-sample>",
    "test_metal_api_gate": "manual:CEYX_METAL_API_GATE=0|1 test_metal_api_gate  (40 fresh processes per arm; verdict per docs/logs/2026-09-05/item3-prereg.md)",
    "test_metal_queue_pool": "manual:test_metal_queue_pool <dng_file>",
    "test_normalize_model_race": "manual:test_normalize_model_race [--selfcheck]  (R4 item 2 race repro; see header Usage)",
    "test_persistent_device_arena": "manual:test_persistent_device_arena  (gpu-copy-elim AC1 §9.1; see header)",
    "test_persistent_device_arena_shrink": "manual:test_persistent_device_arena_shrink  (mem8 T1 SR-1; see header)",
    "test_dng_slot_decommit": "manual:test_dng_slot_decommit  (mem8 T3 SR-6 D1..D6; no args)",
    "test_dng_slot_decommit_real": "manual:test_dng_slot_decommit_real <dng_file>",
    "test_zero_copy_capability_paths": "manual:test_zero_copy_capability_paths [<raw_file>...]",
    "test_concurrent_raw_decode": "manual:test_concurrent_raw_decode  (multi-lane RAW concurrency; see header Usage)",
    "test_concurrent_raw_decode_wrapped": "manual:test_concurrent_raw_decode_wrapped  (unified-wrapped under load; see header Usage)",
    "test_slot_config": "manual:test_slot_config  (R4 item 1 configurable slot cap; no args)",
    "test_codec_roundtrip": "manual:test_codec_roundtrip  (codec Task 8 WebP round trip; no args)",
    "test_codec_jxl": "manual:test_codec_jxl  (codec Task 9 JPEG XL round trip; no args)",
    "test_heif_color": "manual:test_heif_color  (HEIF H1 known-answer colour gate; see header Usage)",
    "test_render_parameter_cache": "manual:test_render_parameter_cache  (gpu-copy-elim AC5 §9.5; see header)",
    "test_linux_vulkan_capability": "manual:test_linux_vulkan_capability  (Linux+Vulkan only; spec A8 capability gate)",
    "pressure_relief_capability_probe": "manual:pressure_relief_capability_probe <library-path>",
    "test_raw_bayer_kernel_android": "manual:adb push + run on device (Android cross-build of test_raw_bayer_kernel)",
    "probe_concurrent_raw_android": "manual:adb push + run on device (Android cross-build of probe_concurrent_raw)",
    "test_stage4_yuv420_output_android": "manual:adb push + run on device (Android cross-build of test_stage4_yuv420_output)",
}

# Runner scripts with no CI/doc reference that survive the 2026-10-02 cleanup
# (design-B-debt §6 Wave 2(i), U4 resolved by decision 2). Documentation only;
# check_test_manifest.py does not read this table.
SCRIPTS: dict[str, str] = {
    "native/tests/run_nikon_he_gate.py": "manual:python3 native/tests/run_nikon_he_gate.py  (Nikon HE gate; SKIP-DECLARED discipline, see docstring)",
    "native/tests/run_parallel_bench.py": "manual:python3 native/tests/run_parallel_bench.py  (parallel decode bench; --self-test-missing-binary self-check)",
    "native/tests/run_timing_harness.py": "manual:python3 native/tests/run_timing_harness.py  (timing harness; see docstring)",
    "native/tests/run_colour_identity.py": "manual:python3 native/tests/run_colour_identity.py  (colour identity; --corrupt-dump-index self-check)",
    "native/tests/check_gpu_producer_width.py": "manual:python3 native/tests/check_gpu_producer_width.py <lowered.stmt> [--require-staged-producer|--forbid-staged-producer] [--max-dispatches N] [--forbid-device-malloc]",
}
