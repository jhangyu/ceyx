#pragma once

#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

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

  // Mutex rework (plan Task 6, R7): non-arena state that a DecodeContext owns
  // and that therefore multiplies with the slot count. Folded in from the
  // Round 4 review item L1 (docs/logs/2026-09-03/round4-review.md, LOW):
  //   stage2_device_dst    W*H uint16   Metal DEVICE memory = 48 MB @6000x4000
  //   handoff.poly3_scratch W*H*3 uint16 host, zero-filled  = 144 MB @6000x4000
  // Unlike the arena these are resident the moment they are sized, not reserved
  // address space, so they belong in the ceiling arithmetic below.
  static constexpr size_t kDecodePerSlotNonArenaBytes =
      static_cast<size_t>(192) * 1024 * 1024;

  // --- Slot-count derivation (R4 item 1, rulings r-1 and r-6) --------------
  //
  // THE USER SETTING IS THE ONLY AUTHORITY. Ruling r-6 is explicit: no code
  // path may impose a memory- or CPU-derived limit on the configured decode
  // lane width. Everything in this section is therefore ADVISORY — a
  // RECOMMENDATION the host may display, never a clamp anything applies.
  //
  // WHAT WAS HERE BEFORE, AND WHY IT WENT. The old arithmetic was
  //   N = min(hardware_concurrency(), kDecodeMemoryCeilingBytes / perSlot)
  //       clamped to [1, <a hardcoded policy maximum of 4>]
  // with a flat 8 GiB (POSIX) / 4 GiB (Windows) budget and a per-slot cost of
  // `kDecodeArenaReserveBytes (1.5 GiB) + kDecodePerSlotNonArenaBytes
  // (192 MiB)` = 1,761,607,680 B. That yields exactly 4 on POSIX and 2 on
  // Windows, so that policy clamp was not even the binding
  // constraint — the ceiling was. Both are gone. Note the category error in the
  // old figure: the 1.5 GiB arena is RESERVED address space (MAP_ANON /
  // MEM_RESERVE), not resident memory, so charging it against a RAM budget
  // over-counted a slot's true residency by roughly 3x.
  //
  // Full write-up: docs/logs/2026-09-05/slot-memory-rederivation.md.

  // Allocation-sanity bound, NOT a parallelism policy and NOT a memory policy.
  // Its only job is to stop a typo'd or hostile request (10^9) from attempting
  // 10^9 DecodeContext constructions. Ruling r-6 requires this to be >= the
  // host slider's maximum so it can never bite inside the range the user can
  // actually select: Halcyon's kMaxDecodeLaneWidth is 8, this is 16.
  static constexpr size_t kAbsoluteMaxDecodeSlots = 16;

  // ---- Advisory recommendation inputs -------------------------------------

  // Bytes of RESIDENT arena a single decode touches, per pixel of the frame:
  //   Stage-3 workspace  W*H*3 uint16 = 6 B/px
  //   Stage-4 RGBA strip W*H*4 uint8  = 4 B/px
  static constexpr size_t kDecodeArenaResidentBytesPerPixel = 10;

  // Recommendation granularity. Per-slot budgets are rounded UP to a multiple
  // of this so the three published resolution classes land on round figures
  // (256 / 640 / 1152 MiB) rather than on arbitrary byte counts.
  static constexpr size_t kDecodeResidentRoundingBytes =
      static_cast<size_t>(128) * 1024 * 1024;

  // The three resolution classes the host settings UI displays, in pixels.
  // 24 MP  =  6000 x 4000  (full-frame 24MP: A7 III, R6, Z6)
  // 61 MP  =  9504 x 6336  (A7R V / A7R IV) — the DEFAULT sizing frame (r-6)
  // 108 MP = 12000 x 9000  (100MP medium format with margin; the frame
  //                         kDecodeArenaReserveBytes was originally sized for)
  static constexpr size_t kDecodePixels24MP = static_cast<size_t>(6000) * 4000;
  static constexpr size_t kDecodePixels61MP = static_cast<size_t>(9504) * 6336;
  static constexpr size_t kDecodePixels108MP =
      static_cast<size_t>(12000) * 9000;

  // Ruling r-6: the default sizing frame for the headline recommendation is
  // 61 MP, not the 108 MP worst case the arena reserve is built for. A 108 MP
  // frame is a medium-format outlier; sizing every recommendation against it
  // understated the honest answer for the cameras people actually shoot.
  static constexpr size_t kDecodeDefaultSizingPixels = kDecodePixels61MP;

  // Resident bytes one slot occupies while decoding a frame of `pixels`:
  // the rounded-up arena working set plus the non-arena state (Metal device
  // dst, poly3 scratch) that is resident the moment it is sized.
  //   24 MP  ->  256 MiB + 192 MiB =  448 MiB
  //   61 MP  ->  640 MiB + 192 MiB =  832 MiB   (the default)
  //  108 MP  -> 1152 MiB + 192 MiB = 1344 MiB
  static constexpr size_t residentBytesPerSlotForPixels(size_t pixels) {
    return (((pixels * kDecodeArenaResidentBytesPerPixel +
              kDecodeResidentRoundingBytes - 1) /
             kDecodeResidentRoundingBytes) *
            kDecodeResidentRoundingBytes) +
           kDecodePerSlotNonArenaBytes;
  }

  // Total physical RAM in bytes, or 0 when it cannot be read.
  static size_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return 0;
    return static_cast<size_t>(st.ullTotalPhys);
#elif defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctl(mib, 2, &bytes, &len, nullptr, 0) != 0) return 0;
    return static_cast<size_t>(bytes);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
#endif
  }

  // RAM left for the OS, the Flutter engine and the payload cache before the
  // decoder's recommendation starts counting.
  static constexpr size_t kDecodeSystemReserveBytes =
      static_cast<size_t>(4) * 1024 * 1024 * 1024;

  // Floor so a machine that reports no memory at all still recommends >= 1.
  static constexpr size_t kDecodeRecommendationFloorBytes =
      static_cast<size_t>(2) * 1024 * 1024 * 1024;

  // Resident budget the recommendation divides against: physical RAM minus the
  // system reserve, floored at 2 GiB. ADVISORY ONLY — nothing clamps to it.
  static size_t decodeRecommendationBudgetBytes() {
    const size_t phys = physicalMemoryBytes();
    if (phys <= kDecodeSystemReserveBytes) {
      return kDecodeRecommendationFloorBytes;
    }
    const size_t budget = phys - kDecodeSystemReserveBytes;
    return budget < kDecodeRecommendationFloorBytes
               ? kDecodeRecommendationFloorBytes
               : budget;
  }

  // How many concurrent slots this machine is RECOMMENDED to run for a frame
  // of `pixels`. Reported to the host for display next to the lane-width
  // slider. Bounded to [1, kAbsoluteMaxDecodeSlots] only so the displayed
  // number stays inside the range the slider can express.
  //
  // NOTHING IN THIS PROCESS CLAMPS AGAINST THIS VALUE (ruling r-6). If a user
  // selects a width above it, that width is what the pool runs at.
  static size_t decodeRecommendedSlotsForPixels(size_t pixels) {
    const size_t perSlot = residentBytesPerSlotForPixels(pixels);
    if (perSlot == 0) return 1;
    size_t n = decodeRecommendationBudgetBytes() / perSlot;
    if (n > kAbsoluteMaxDecodeSlots) n = kAbsoluteMaxDecodeSlots;
    if (n < 1) n = 1;
    return n;
  }

  // The headline recommendation, at the 61 MP default sizing frame (r-6).
  static size_t decodeRecommendedSlots() {
    return decodeRecommendedSlotsForPixels(kDecodeDefaultSizingPixels);
  }

  // Ruling r-8 (user, 2026-09-05). The parallelism used when NO consumer has
  // configured one. Four, flat — not derived from cores, not derived from
  // memory.
  //
  // THIS IS A DEFAULT, NOT A CAP. It is the starting value only. The moment
  // any consumer calls dng_decode_configure_slots(), that value is followed
  // exactly, end to end, and this constant plays no further part — it is never
  // a min(), never a ceiling, and never consulted again. The only bound on a
  // configured value is the [1, kAbsoluteMaxDecodeSlots] allocation-sanity
  // range, which sits above the host slider maximum and cannot bite.
  //
  // Do not rename this to anything resembling "max"/"limit"/"cap", and do not
  // move it next to kAbsoluteMaxDecodeSlots: the constant it replaces
  // (kMaxDecodeSlots = 4) WAS a cap, it had the same value, and a future
  // reader who conflates the two would silently reinstate the defect this
  // item removed.
  static constexpr size_t kDefaultDecodeSlots = 4;

  // The N the pool constructs at before any host configures one. See
  // kDefaultDecodeSlots immediately above for why this is deliberately a flat
  // constant rather than a derivation.
  static size_t decodeSlotCount() { return kDefaultDecodeSlots; }

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
