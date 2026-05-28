#pragma once

#include <cstdint>
#include <cstdlib>

// ---------------------------------------------------------------------------
// PipelineConfig — single source of truth for runtime env switches.
//
// Categories (Phase 11 architecture cleanup, Round 1 P1):
//   1. RouteConfig (formerly `Debug`):
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
//              Stage 2 Metal pre-warm shot — leave ON in production),
//              DNG_STAGE2_SDK_TIMING (vendor — read in
//              third_party/dng_sdk/source/, NOT modified here).
//   3. ResearchConfig:
//        Deprecated / experimental flags retained for A/B parity research.
//        Do NOT consult in production paths; slated for removal in Round 2.
//        Envs: DNG_WARP_PRECOMPUTED_COORDS (dng_warp_halide.cpp).
//
// Notes:
//   * `PipelineConfig::RouteConfig` is the renamed `Debug` struct type. The
//     field is still named `.debug` for source-compatibility with existing
//     call sites in dng_pipeline_v2.cpp / dng_render_halide.cpp; both names
//     refer to the same struct (see `using Debug = RouteConfig;`). The field
//     name will be renamed in a follow-up sweep.
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

  // Back-compat alias so existing call sites that reference
  // `PipelineConfig::Debug` keep compiling. Prefer `RouteConfig` in new code.
  using Debug = RouteConfig;

  RouteConfig debug;  // field name kept for source-compat (Round 2 will rename to `route`).

  struct Threads {
    uint32_t area_threads = 0;
  } threads;

  static PipelineConfig loadFromEnv() {
    PipelineConfig config;
    config.debug.fused_demosaic_warp = !envExplicitZero("DNG_FUSED_DEMOSAIC_WARP");
    config.debug.stage3_stage4_device_handoff = !envExplicitZero("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");
    config.debug.stage2_stage4_device_handoff = !envExplicitZero("DNG_STAGE2_STAGE4_DEVICE_HANDOFF");
    config.debug.stage2_ol2_halide = stage2OL2HalideEnabled();

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
