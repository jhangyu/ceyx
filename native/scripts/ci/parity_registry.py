"""Platform-parity registry (PARITY.md). DATA ONLY; read by check_platform_parity.py.

Every tracked in-scope file containing a platform conditional is listed here.
role:
  adapter      same observable contract on every platform, OS API bound per platform
  accelerator  optional speed-up on a subset of platforms; output-identical and holds
               nothing past the call (PARITY.md clause 3)
  parked       a platform divergence outside the memory-reclamation campaign; reported
               to the user at campaign close, not fixed here
  fork-host    file still hosts a campaign fork listed in open_forks
open_forks: campaign fork ids (plan section 4). A milestone deletes ONLY its own ids,
in the same commit that removes the fork. Campaign end state: no open_forks anywhere.
"""

REQUIRE_CLOSED = False

REGISTRY: dict[str, dict] = {
    "native/include/ceyx_ffi_export.h": {
        "guard_lines": 1, "role": "adapter",
        "contract": "symbol export decoration (dllexport vs visibility default)"},
    "native/include/ceyx_utf8_path.h": {
        "guard_lines": 2, "role": "adapter",
        "contract": "open a UTF-8 path (wide-char API on Windows)"},
    "native/include/dng_pipeline_config.h": {
        "guard_lines": 4, "role": "adapter",
        "contract": "physicalMemoryBytes: the single physical-RAM query (audit A7)"},
    "native/src/ffi/ceyx_decode_into_ffi.cpp": {
        "guard_lines": 5, "role": "adapter",
        "contract": "aligned alloc/free adapter (A4)"},
    "native/src/ffi/heap_page_return.cpp": {
        "guard_lines": 8, "role": "adapter",
        "contract": "return free heap pages to the OS; ran/unavailable telemetry (TC-1455)"},
    "native/src/ffi/dng_ffi_api.cpp": {
        "guard_lines": 5, "role": "parked",
        "contract": "Android-only Vulkan pipeline-cache persistence (:43-90); not reclamation"},
    "native/src/pipeline/decode_context.h": {
        "guard_lines": 7, "role": "fork-host",
        "contract": "DecodeArena page size/release adapter (A5); commit model is fork B5",
        "open_forks": ["B5"]},
    "native/src/pipeline/dng_copy_lock.cpp": {
        "guard_lines": 1, "role": "adapter",
        "contract": "Metal concurrency correctness (A9); no memory retention"},
    "native/src/pipeline/dng_halide_device.cpp": {
        "guard_lines": 8, "role": "adapter",
        "contract": "Halide device interface binding and idle device-memory release (Vulkan pool release / Metal parameter cache); identical DngDeviceReleaseResult semantics"},
    "native/src/pipeline/dng_metal_api_gate.h": {
        "guard_lines": 1, "role": "adapter",
        "contract": "Metal API serialisation gate (A9)"},
    "native/src/pipeline/dng_metal_context.cpp": {
        "guard_lines": 3, "role": "adapter",
        "contract": "Metal queue pool / device context (A9)"},
    "native/src/pipeline/dng_pipeline.cpp": {
        "guard_lines": 6, "role": "parked",
        "contract": "Windows include block (adapter) + Android-only verbose timing lines; not reclamation"},
    "native/src/pipeline/dng_render_halide.cpp": {
        "guard_lines": 11, "role": "accelerator",
        "contract": "Metal zero-copy destination wrap (A11); Android prewarm caches parked (A10)"},
    "native/src/pipeline/dng_warp_halide.cpp": {
        "guard_lines": 11, "role": "fork-host",
        "contract": "Android prewarm caches parked (A10); ZeroCoordBuffer commit model is fork A6",
        "open_forks": ["A6"]},
    "native/src/pipeline/libraw_frontend.cpp": {
        "guard_lines": 1, "role": "adapter",
        "contract": "LibRaw open on Windows wide path"},
    "native/src/pipeline/raw_gpu_pipeline.cpp": {
        "guard_lines": 7, "role": "accelerator",
        "contract": "Metal zero-copy wrap (A11): output-identical, holds nothing past the call"},
    "native/src/pipeline/raw_gpu_timing_probe.cpp": {
        "guard_lines": 1, "role": "adapter",
        "contract": "GPU timing probe backend binding"},
    "native/src/pipeline/raw_persistent_device_arena.cpp": {
        "guard_lines": 3, "role": "accelerator",
        "contract": "Metal zero-copy lane arena (A11 family); released by funnel step 1"},
    "native/src/pipeline/render_parameter_upload_cache.cpp": {
        "guard_lines": 1, "role": "accelerator",
        "contract": "Metal C3 parameter cache; released by funnel step 3"},
    "plugin/lib/src/dng_bindings.dart": {
        "guard_lines": 10, "role": "adapter",
        "contract": "native library file name and loader path per OS"},
    "plugin/lib/src/gpu_shutdown.dart": {
        "guard_lines": 1, "role": "adapter",
        "contract": "exit-hook source chosen by dart:ui availability (Flutter vs headless), not by OS; identical behaviour on every OS (6c91791, user T-G exemption; PARITY.md item 10)"},
}
