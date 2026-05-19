#pragma once

#include <cstdint>
#include <cstdlib>

struct PipelineConfig {
  struct Debug {
    bool fused_demosaic_warp = true;
    bool stage3_stage4_device_handoff = true;
    bool stage2_stage4_device_handoff = true;
    // Phase 10 Sprint C3: enable Halide GPU dispatch for Stage 2 OpcodeList2
    // MapPolynomial. Bridge implementation uses getenv("DNG_STAGE2_OL2_HALIDE")
    // as source-of-truth; this field is provided for future caller access.
    bool stage2_ol2_halide = true;
  } debug;

  struct Threads {
    uint32_t area_threads = 0;
  } threads;

  static PipelineConfig loadFromEnv() {
    PipelineConfig config;
    config.debug.fused_demosaic_warp = !envExplicitZero("DNG_FUSED_DEMOSAIC_WARP");
    config.debug.stage3_stage4_device_handoff = !envExplicitZero("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");
    config.debug.stage2_stage4_device_handoff = !envExplicitZero("DNG_STAGE2_STAGE4_DEVICE_HANDOFF");
    config.debug.stage2_ol2_halide = !envExplicitZero("DNG_STAGE2_OL2_HALIDE");

    config.threads.area_threads = envPositiveU32("DNG_AREA_THREADS");
    return config;
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
