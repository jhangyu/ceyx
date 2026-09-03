#pragma once

#include <cstdint>
#include <cstdlib>

// ---------------------------------------------------------------------------
// PipelineConfig — single source of truth for runtime env switches.
//
// Categories (Phase 11 architecture cleanup, Round 1 P1):
//   1. RouteConfig:
//        Production-relevant kill-switches and routing toggles. Default ON;
//        flip OFF to take a fallback path. Read at decode entry.
//        Envs: DNG_AREA_THREADS (tuning, see ThreadConfig),
//              DNG_FUSED_DEMOSAIC_WARP,
//              DNG_STAGE3_STAGE4_DEVICE_HANDOFF,
//              DNG_STAGE2_STAGE4_DEVICE_HANDOFF,
//              DNG_STAGE2_OL2_HALIDE (source-of-truth lives here; see
//              stage2Opcodelist2HalideEnabled() and dng_opcodelist2_halide.cpp),
//              DNG_FUSE_RGBA (7.1 testing/rollback override for the fused
//              RGBA8 output feature; =0 forces the legacy RGB8 path. Consulted
//              by dng_pipeline_decode_to_rgb to set the internal
//              fuse_rgba_output toggle; see route.fuse_rgba below).
//   2. DiagnosticConfig:
//        Observability / timing flags. Default OFF; flip ON to log timings.
//        These are read lazily at their call sites (cached via static const)
//        to avoid impacting hot paths; they are catalogued here for
//        discoverability rather than centrally loaded.
//        Envs: DNG_STAGE1_TIMING (concurrent_dng_host.h),
//              DNG_MAP_POLY_TIMING (dng_opcodelist2_halide.cpp),
//              DNG_STAGE2_OL2_PREWARM (route/diagnostic hybrid; gates the
//              Stage 2 Metal pre-warm shot — leave ON in production. Read
//              at ol2_prewarm_enabled() in dng_opcodelist2_halide.cpp; the
//              actual prewarm dispatch happens in
//              dng_pipeline.cpp::dng_pipeline_decode_to_rgb just
//              before BuildStage2Image (see W6-6 / TD-29)),
//              DNG_STAGE2_SDK_TIMING (vendor — read in
//              third_party/dng_sdk/source/, NOT modified here),
//              DNG_GPU_BACKEND (route: override GPU backend — "metal"/"vulkan";
//              default auto-detect per platform. Read once at first GPU dispatch via
//              dng_halide_device.cpp; see dng_halide_device.h).
//              DNG_PIPELINE_VERBOSE (gates [Stage4-Diag]/[Stage4-Perf]/
//              [Pipeline] GPU backend informational stderr banners. Lazy-cached
//              via pipelineVerbose() in dng_pipeline.cpp, with a separate
//              TU-local cache of the same env in dng_render_halide.cpp by
//              design, since that helper isn't exported via a header).
//   3. ResearchConfig:
//        Deprecated / experimental flags retained for A/B parity research.
//        Do NOT consult in production paths; default builds compile these out
//        unless their dedicated diagnostic CMake option is enabled.
//        Envs: DNG_WARP_PRECOMPUTED_COORDS — historically consumed by the
//              precomputed-coords WarpRectilinear DIAG bridge in
//              dng_warp_halide.cpp. That bridge has been removed from the
//              production decode path; no source consults this env anymore.
//              The src/research/ generators (RectilinearWarpStrictFloatGenerator /
//              RectilinearWarpDebugGenerator) and their opt-in CMake option/target
//              are retained for standalone A/B research only.
//
// Notes:
//   * Diagnostic and Research envs are intentionally NOT loaded by
//     loadFromEnv() to preserve their existing lazy-init / call-site cache
//     semantics. Moving them would risk altering first-call timing windows.
// ---------------------------------------------------------------------------

struct PipelineConfig {
  // Category 1: production routing / kill-switches.
  struct RouteConfig {
    bool fused_demosaic_warp = true;
    bool stage3_stage4_device_handoff = true;
    bool stage2_stage4_device_handoff = true;
    // Phase 10 Sprint C3: enable Halide GPU dispatch for Stage 2 OpcodeList2
    // MapPolynomial. dng_opcodelist2_halide.cpp consults
    // PipelineConfig::stage2Opcodelist2HalideEnabled() for the canonical value
    // (see source-of-truth: dng_pipeline_config.h comment there).
    bool stage2_opcodelist2_halide = true;
    // 7.1: testing/rollback override for the fused RGBA8 output feature.
    // DNG_FUSE_RGBA=0 forces the legacy RGB8 path; default true. Consulted by
    // dng_pipeline_decode_to_rgb, which funnels this into fuse_rgba_output.
    bool fuse_rgba = true;
  };

  RouteConfig route;

  // W7-B (P15): when true, the Stage4 bridge writes a fused interleaved RGBA8
  // output (alpha=255) directly into the caller buffer instead of interleaved
  // RGB8, eliminating the separate FFI rgb_to_rgba pass and one ~72MB
  // read/write of the RGB intermediate. Set only by
  // dng_pipeline_decode_to_rgb on Android (Vulkan host-side repack path);
  // test_decode and macOS leave it false so RGB output is preserved. This is an
  // internal output-format toggle, NOT an env-driven route kill-switch, so it
  // lives outside RouteConfig and is not read by loadFromEnv().
  bool fuse_rgba_output = false;

  struct Threads {
    uint32_t area_threads = 0;
  } threads;

  // Default area-task thread count used when DNG_AREA_THREADS is unset / <= 0.
  // Named constant so the production decode path and the test harness
  // (test_decode.cpp kOptimizedAreaThreads) cannot drift apart.
  // Q3a (Round 2 perf diag): this is a fixed upper bound for the largest
  // supported device tier, not a target — ConcurrentDngHost::
  // PerformAreaTaskThreads() (concurrent_dng_host.h) clamps whatever value
  // resolves here (or via DNG_AREA_THREADS) down to
  // std::thread::hardware_concurrency(), so machines with fewer logical
  // cores never over-provision std::async workers per decode.
  static constexpr uint32_t kDefaultAreaThreads = 20;

  // Mutex rework (plan Task 4): address space reserved per DecodeContext for
  // its bump arena (decode_context.h). RESERVED, not resident — pages arrive
  // on first touch (MAP_ANON / MEM_RESERVE|MEM_COMMIT), so an over-generous
  // reserve costs nothing at rest.
  //
  // Arithmetic (recorded here because Task 6's slot count and the §6.3 memory
  // disclosure both consume it). Sizing frame = 12000 x 9000 = 108,000,000 px,
  // chosen to cover the largest still formats we intend to support (100 MP
  // medium format is 11656 x 8742) with margin:
  //   Stage-3 workspace  W*H*3 uint16 = 108e6 * 6 = 648,000,000 B
  //   Stage-4 RGBA strip W*H*4 uint8  = 108e6 * 4 = 432,000,000 B
  //   subtotal                                   = 1,080,000,000 B
  //   + 64-byte alignment padding and Task 5 headroom
  //   round up to 1.5 GiB                        = 1,610,612,736 B
  // A decode whose frame exceeds this gets nullptr from arena.allocate() and
  // takes its existing allocation-failure path; the arena never grows, because
  // a growing arena would move memory a caller is still holding.
  static constexpr size_t kDecodeArenaReserveBytes =
      static_cast<size_t>(1536) * 1024 * 1024;

  static PipelineConfig loadFromEnv() {
    PipelineConfig config;
    config.route.fused_demosaic_warp =
        !envExplicitZero("DNG_FUSED_DEMOSAIC_WARP");
    config.route.stage3_stage4_device_handoff =
        !envExplicitZero("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");
    config.route.stage2_stage4_device_handoff =
        !envExplicitZero("DNG_STAGE2_STAGE4_DEVICE_HANDOFF");
    config.route.stage2_opcodelist2_halide = stage2Opcodelist2HalideEnabled();
    config.route.fuse_rgba = !envExplicitZero("DNG_FUSE_RGBA");

    config.threads.area_threads = envPositiveU32("DNG_AREA_THREADS");
    return config;
  }

  // Canonical reader for DNG_STAGE2_OL2_HALIDE. Lives here so the Stage 2
  // OpcodeList2 bridge (dng_opcodelist2_halide.cpp) and PipelineConfig agree on
  // a single source of truth. Lazy-cached so repeated calls are cheap.
  static bool stage2Opcodelist2HalideEnabled() {
    static const bool cached = !envExplicitZero("DNG_STAGE2_OL2_HALIDE");
    return cached;
  }

private:
  static bool envExplicitZero(const char *key) {
    const char *v = std::getenv(key);
    return v && v[0] == '0';
  }

  static uint32_t envPositiveU32(const char *key) {
    const char *v = std::getenv(key);
    if (!v || !v[0]) {
      return 0;
    }
    const long parsed = std::strtol(v, nullptr, 10);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
  }
};
