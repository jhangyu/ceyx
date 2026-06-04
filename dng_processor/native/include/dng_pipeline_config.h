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
//              stage2OL2HalideEnabled() and dng_opcodelist2_halide.cpp).
//   2. DiagnosticConfig:
//        Observability / timing flags. Default OFF; flip ON to log timings.
//        These are read lazily at their call sites (cached via static const)
//        to avoid impacting hot paths; they are catalogued here for
//        discoverability rather than centrally loaded.
//        Envs: DNG_STAGE1_TIMING (ConcurrentDngHost.h),
//              DNG_MAP_POLY_TIMING (dng_opcodelist2_halide.cpp),
//              DNG_STAGE2_OL2_PREWARM (route/diagnostic hybrid; gates the
//              Stage 2 Metal pre-warm shot — leave ON in production. Read
//              at ol2_prewarm_enabled() in dng_opcodelist2_halide.cpp; the
//              actual prewarm dispatch happens in
//              dng_pipeline_v2.cpp::dng_pipeline_v2_decode_to_rgb just
//              before BuildStage2Image (see W6-6 / TD-29)),
//              DNG_STAGE2_SDK_TIMING (vendor — read in
//              third_party/dng_sdk/source/, NOT modified here),
//              DNG_GPU_BACKEND (route: override GPU backend — "metal"/"vulkan";
//              default auto-detect per platform. Read once at first GPU dispatch via
//              dng_halide_device.cpp; see dng_halide_device.h).
//   3. ResearchConfig:
//        Deprecated / experimental flags retained for A/B parity research.
//        Do NOT consult in production paths; default builds compile these out
//        unless their dedicated diagnostic CMake option is enabled.
//        Envs: DNG_WARP_PRECOMPUTED_COORDS (research-only switch consumed by
//              the precomputed-coords WarpRectilinear AOT path in
//              src/research/; see Phase 8.1.6 D-1 / Gotcha #51. Production
//              builds must NOT depend on this — registry-only since W6-6).
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
    // PipelineConfig::stage2OL2HalideEnabled() for the canonical value (see
    // // source-of-truth: dng_pipeline_config.h comment there).
    bool stage2_ol2_halide = true;
  };

  RouteConfig route;

  struct Threads {
    uint32_t area_threads = 0;
  } threads;

  // Default area-task thread count used when DNG_AREA_THREADS is unset / <= 0.
  // Named constant so the production decode path and the test harness
  // (test_decode.cpp kOptimizedAreaThreads) cannot drift apart.
  static constexpr uint32_t kDefaultAreaThreads = 20;

  static PipelineConfig loadFromEnv() {
    PipelineConfig config;
    config.route.fused_demosaic_warp =
        !envExplicitZero("DNG_FUSED_DEMOSAIC_WARP");
    config.route.stage3_stage4_device_handoff =
        !envExplicitZero("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");
    config.route.stage2_stage4_device_handoff =
        !envExplicitZero("DNG_STAGE2_STAGE4_DEVICE_HANDOFF");
    config.route.stage2_ol2_halide = stage2OL2HalideEnabled();

    config.threads.area_threads = envPositiveU32("DNG_AREA_THREADS");
    return config;
  }

  // Canonical reader for DNG_STAGE2_OL2_HALIDE. Lives here so the Stage 2 OL2
  // bridge (dng_opcodelist2_halide.cpp) and PipelineConfig agree on a single
  // source of truth. Lazy-cached so repeated calls are cheap.
  static bool stage2OL2HalideEnabled() {
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
