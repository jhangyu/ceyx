#pragma once

#include <cstdint>
#include <cstdlib>

struct PipelineConfig {
  struct Timing {
    bool render_halide = false;
    bool warp_halide = false;
    bool demosaic_halide = false;
    bool demosaic_warp_halide = false;
    bool halide_readback_split = false;
  } timing;

  struct Debug {
    bool warp_bit_exact = false;
    bool render_bit_exact = false;
    bool render_bit_exact_specified = false;
    bool render_disable_full = false;
    bool render_force_full = false;
    bool render_disable_huesat = false;
    bool render_disable_look = false;
    bool demosaic_aot = true;
    bool demosaic_bit_exact = false;
    bool fused_demosaic_warp = true;
    bool stage3_stage4_device_handoff = true;
  } debug;

  struct Threads {
    uint32_t area_threads = 0;
    uint32_t render_area_threads = 0;
    uint32_t demosaic_threads = 0;
  } threads;

  static PipelineConfig loadFromEnv() {
    PipelineConfig config;
    config.timing.render_halide = envFlag("DNG_RENDER_HALIDE_TIMING");
    config.timing.warp_halide = envFlag("DNG_WARP_HALIDE_TIMING");
    config.timing.demosaic_halide = envFlag("DNG_DEMOSAIC_HALIDE_TIMING");
    config.timing.demosaic_warp_halide = envFlag("DNG_DEMOSAIC_WARP_HALIDE_TIMING");
    config.timing.halide_readback_split = envFlag("DNG_HALIDE_READBACK_SPLIT_TIMING");

    // Debug-only compatibility controls. Production dispatch should not add
    // scattered env sniffing for these; route all reads through this config.
    config.debug.warp_bit_exact = envFlag("DNG_WARP_BIT_EXACT");
    config.debug.render_bit_exact = envFlag("DNG_RENDER_BIT_EXACT");
    config.debug.render_bit_exact_specified = envPresent("DNG_RENDER_BIT_EXACT");
    config.debug.render_disable_full = envFlag("DNG_RENDER_HALIDE_DISABLE_FULL");
    config.debug.render_force_full = envFlag("DNG_RENDER_HALIDE_FORCE_FULL");
    config.debug.render_disable_huesat = envFlag("DNG_RENDER_DISABLE_HUESAT");
    config.debug.render_disable_look = envFlag("DNG_RENDER_DISABLE_LOOK");
    config.debug.demosaic_aot = !envExplicitZero("DNG_DEMOSAIC_AOT");
    config.debug.demosaic_bit_exact = envFlag("DNG_DEMOSAIC_BIT_EXACT");
    config.debug.fused_demosaic_warp = !envExplicitZero("DNG_FUSED_DEMOSAIC_WARP");
    config.debug.stage3_stage4_device_handoff = !envExplicitZero("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");

    config.threads.area_threads = envPositiveU32("DNG_AREA_THREADS");
    config.threads.render_area_threads = envPositiveU32("DNG_RENDER_AREA_THREADS");
    config.threads.demosaic_threads = envPositiveU32("DNG_DEMOSAIC_THREADS");
    return config;
  }

private:
  static bool envFlag(const char *key) {
    const char *v = std::getenv(key);
    return v && v[0] && v[0] != '0';
  }

  static bool envExplicitZero(const char *key) {
    const char *v = std::getenv(key);
    return v && v[0] == '0';
  }

  static bool envPresent(const char *key) {
    const char *v = std::getenv(key);
    return v != nullptr;
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
