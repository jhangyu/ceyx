#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class dng_host;
class dng_negative;

struct DngPipelineResult {
  // W7-B (P15): fused interleaved RGBA8 output (alpha=255). WP1 phase 3: this
  // is now the only Stage4 output field — RGB8 output no longer exists.
  // WP5: the buffer is ALWAYS the caller's. The library neither allocates nor
  // frees it, on any path.
  uint8_t* rgba_ptr = nullptr;
  size_t   rgba_size = 0;
  // WP10: when true, rgba_ptr points at a CALLER-OWNED buffer, which must never
  // be freed by the library. WP5: with the output pool deleted there is no
  // other kind of buffer, so on every surviving decode route this is true; the
  // flag is retained because it is what the caller-binding step sets, and
  // because `rgba_ptr == dst` remains the caller's proof rather than its
  // assumption.
  //
  // This field is an INPUT to dng_pipeline_decode_to_rgb_sized, unlike every
  // other field on this struct, which is an output. That is why that function
  // carries it (and the rgba_ptr/rgba_size it describes) across its own result
  // reset instead of clearing it with the rest.
  //
  // This struct is INTERNAL: it carries no FFI static_assert and crosses no ABI
  // boundary, unlike DngResult.
  bool     rgba_caller_owned = false;
  uint32_t width = 0;
  uint32_t height = 0;
  double decode_ms = 0.0;
  double process_ms = 0.0;
  int32_t error_code = 0;
};

// WP5: the process-scoped, checkout-style RGBA output pool and its three
// accessors are DELETED. Distinct per-decode buffers -- the property the pool
// existed to provide -- are now guaranteed by the caller supplying its own
// destination for every decode.

// Mutex rework (plan Task 6, spec R7/R8). Admission control accessors.
//
// dng_decode_slot_count()      — the configured N (constant after first use).
// dng_decode_in_flight_count() — decodes holding a slot right now. Read per
//                                area task by ConcurrentDngHost to divide the
//                                nested fan-out; never cached.
// dng_decode_max_in_flight_observed() — high-water of the above, for the gate
//                                that asserts the bound was never exceeded.
// dng_decode_arena_high_water_bytes() — largest bump-arena offset any slot has
//                                reached; the RESIDENT figure the memory
//                                disclosure needs, as opposed to the reserve.
//
// These are internal instrumentation. (R4 item 1 note: dng_ffi_api.h is no
// longer "unchanged" — it now carries the C ABI the host uses to CONFIGURE the
// slot count. These remain the internal, C++-linkage half.)
size_t dng_decode_slot_count();
size_t dng_decode_in_flight_count();
size_t dng_decode_max_in_flight_observed();
size_t dng_decode_arena_high_water_bytes();

// --- R4 item 1: configurable slot pool -----------------------------------

// Live reconfiguration of the slot pool (ruling r-5: grow at once, shrink by
// tightening admission and reclaiming slots as decodes finish, never
// pre-empting).
//
// Clamping is the CALLER's job. dng_ffi_api.cpp bounds the request only by the
// allocation-sanity constant, per ruling r-6 — there is deliberately no
// memory- or CPU-derived clamp anywhere on the propagation path. This applies
// whatever it is given, floored at 1.
//
// Exists as a C++-linkage bridge so the FFI translation unit never has to
// include decode_context.h, which pulls the DNG SDK.
void dng_decode_resize_slots(size_t n);

// Contexts physically allocated right now. Equals dng_decode_slot_count()
// except inside a narrowing window, where surplus contexts are still checked
// out and therefore not yet destroyable. Exposed so a gate can prove the pool
// actually shed contexts rather than only tightening its admission predicate.
size_t dng_decode_physical_slot_count();

// Lock-free, side-effect-free read of the configured slot count.
//
// For callers already holding their own mutex on a per-decode path — e.g. the
// Metal queue pool (dng_metal_context.cpp, inside pool_lock()). The locking
// accessor above would nest the slot-pool mutex underneath theirs AND could
// construct the entire pool as a side effect of a bookkeeping question.
size_t dng_decode_slot_count_relaxed();

// --- mem8 T3 (SR-6): DNG-route idle decommit -----------------------------
//
// SCOPE HONESTY, binding on anything that reports these numbers: the saving
// here is STRUCTURAL and CONDITIONAL. No DNG corpus exists on the development
// host (OQ-3) and DNG-vs-RAW additivity is UNTESTED. These entries are covered
// by mechanism-level gates driving the arena synthetically, not by a measured
// decode. Do not quote a DNG saving from them.

// True once the decode slot pool has ACTUALLY been constructed. Reading it can
// never construct one, which is why the idle funnel asks this rather than
// dng_decode_slot_count(): a pure-RAW session must not mmap 8 x 1.5 GiB to
// answer an idle timer's bookkeeping question.
//
// NOT interchangeable with "g_configured_slots != 0". That atomic is written by
// only two of the pool accessor's callers; the decode path constructs the pool
// without setting it, so it reads 0 on a process that has decoded DNGs. See the
// definition's comment in dng_pipeline.cpp.
bool dng_decode_slot_pool_exists();

// Instrumentation only: the RAW published slot count, 0 meaning "never
// published", with no default fallback and no side effect. This is the frozen
// spec's REJECTED predicate, exposed so the guard swap can be asserted
// mechanically instead of argued — D8 constructs the pool through a
// non-publishing path and pins this to 0 while the accessor above reads true.
size_t dng_decode_published_slot_count_raw();

// Idle-release arenas and scratch of FREE contexts in excess of `floor`,
// keeping the first `floor` warm. Returns bytes released.
//
// floor == 0 is legal (release every free context). Fewer free contexts than
// the floor is a NO-OP RETURNING 0, and that is SUCCESS, not failure — the same
// floor semantics as the lane arena's shrink, whose full contract lives on
// raw_persistent_device_arena_shrink_to_lane_floor and is not restated here.
//
// Checked-out contexts are SKIPPED, never deferred: release_idle_state() issues
// a Metal device free, so the caller must guarantee decode quiescence and the
// free-list rule is the backstop, not a lock.
//
// A second entry on the slot pool but NOT a second lane-width policy: it
// changes no target_, admits nothing and erases no context.
// dng_decode_resize_slots remains the single width funnel.
size_t dng_decode_decommit_free_slots_to_floor(size_t floor);

// Instantaneous committed arena bytes across every context. DISTINCT from
// dng_decode_arena_high_water_bytes(), which is monotonic and is the existing
// disclosure figure — D-P1-4 pins high-water to stay UNCHANGED across a
// decommit so that disclosure does not silently change meaning.
size_t dng_decode_committed_context_bytes();

// Decommit bookkeeping. A call count that moves with zero contexts touched is
// the degenerate floor case (success); a call count that never moves means the
// idle path is not wired at all.
size_t dng_decode_decommit_call_count();
size_t dng_decode_contexts_decommitted_count();

// Round 5 review F2: the same bound observed from OUTSIDE the pool's
// bookkeeping. dng_decode_max_in_flight_observed() reads a counter the pool
// maintains against its own free list, so it cannot exceed the slot count by
// construction — asserting on it is unfalsifiable. These two are maintained by
// the decode body itself:
//   dng_decode_body_max_in_flight() — high-water of decodes actually executing.
//   dng_decode_body_alias_events()  — times a decode found its DecodeContext
//                                     already occupied, i.e. the pool handed
//                                     one context to two callers. Must be 0.
size_t dng_decode_body_max_in_flight();
size_t dng_decode_body_alias_events();

// Round 7 task #2: Stage-3 workspace exclusivity, and the coverage counter that
// makes its absence loud.
//
// WHY THIS EXISTS RATHER THAN A PIXEL COMPARE. On 2026-09-03 the concurrent
// pixel-compare gate (G3) was run against a deliberately SHARED process-wide
// Stage-3 workspace and came back byte-identical, i.e. green on a defect it was
// built to catch. Root cause (docs/logs/2026-09-04/r7g3-root-cause.md): only
// Bayer files reach the Stage-3 workspace at all — decodeStages branches on
// isBayer — and the corpus held exactly ONE Bayer file, so every decode that
// could touch the shared workspace was decoding the SAME image and wrote
// byte-identical values into it. No delay setting and no repeat count could
// have made that arm go red.
//
//   dng_stage3_workspace_alias_events() — times a decode found the Stage-3
//        workspace it was handed already registered to a DIFFERENT live decode.
//        Must be 0. This detects the ALIASING ITSELF, so unlike a pixel compare
//        it does not depend on the two decodes holding different data, on the
//        race window being hit, or on the delay knob. Nested acquisitions
//        within one decode (the fused path and the Bayer path both call
//        prepareStage3WorkspacePtr for one decode) are refcounted by owner and
//        are NOT alias events.
//   dng_stage3_workspace_registrations() — how many decodes reached the Stage-3
//        workspace at all. A gate that asserts alias_events == 0 while this is
//        0 has asserted nothing; the gate must FAIL on zero rather than read an
//        unexercised route as a clean pass. That silent pass is the defect that
//        produced the 2026-09-03 green.
size_t dng_stage3_workspace_alias_events();
size_t dng_stage3_workspace_registrations();

struct DngPipelineStage3Timing {
  double extract_stage2_ms = 0.0;
  double make_image_ms = 0.0;
  double workspace_acquire_ms = 0.0;
  double demosaic_ms = 0.0;
  double fused_demosaic_warp_ms = 0.0;
  double fast_warp_setup_ms = 0.0;
  double inject_put_ms = 0.0;
  double apply_opcode3_ms = 0.0;
  double sdk_build_ms = 0.0;
  double total_ms = 0.0;
};

bool dng_pipeline_decode_to_rgb(const char *file_path,
                                   DngPipelineResult &result);

// R2 sized decode. max_dim caps the OUTPUT long edge; the aspect ratio is
// preserved by the SDK's own MaximumSize logic. max_dim <= 0 means
// full resolution and is exactly equivalent to dng_pipeline_decode_to_rgb
// (which forwards here with 0).
//
// Only the Bayer/CFA Halide path can produce a scaled result on the GPU. A
// sized request on any other path logs and falls back to full resolution
// rather than returning a cropped or failed decode.
bool dng_pipeline_decode_to_rgb_sized(const char *file_path,
                                         int32_t max_dim,
                                         DngPipelineResult &result);

// WP10 (A3.2): as dng_pipeline_decode_to_rgb_sized, but the RGBA output is
// written into the CALLER's buffer. Binds dst AFTER the internal result reset,
// which is precisely why it is a parameter and not a pre-set field on `result`:
// the reset would wipe a pre-set pointer and the decode would quietly fall back
// to the pool while the caller believed it owned the buffer.
//
// Ownership: dst is never freed by the library, on ANY exit path. WP5: this
// used to be enforced by constructing the checkout guard inactive; the guard
// and the pool it returned buffers to are both deleted, so there is no longer
// any code path that could free dst.
bool dng_pipeline_decode_to_rgb_into(const char *file_path, int32_t max_dim,
                                     uint8_t *dst, size_t dst_capacity,
                                     DngPipelineResult &result);

// Productionization plan §1.4 (Task 3, AMENDED by reconciliation 2/3): the
// oriented sibling of dng_pipeline_decode_to_rgb_into. exif_orientation is an
// EXPLICIT PARAMETER, not a PipelineConfig field — PipelineConfig is
// env-loaded route/settings state (loadFromEnv()), and per-call data placed
// there invites a future caller to assume it is stable and cache it.
// exif_orientation: EXIF tag values 1..8; any other value is treated as 1
// (identity), matching ceyx_orient_rgba's contract. dng_pipeline_decode_to_rgb_into
// forwards here with exif_orientation = 1, so the two entries share one body
// and can never drift (§1.4, Step 3.2).
bool dng_pipeline_decode_to_rgb_into_oriented(const char *file_path, int32_t max_dim,
                                              uint8_t *dst, size_t dst_capacity,
                                              int32_t exif_orientation,
                                              DngPipelineResult &result);

// WP10: metadata-only output-extent probe. Same sizing rules as
// dng_pipeline_decode_to_rgb_sized (the same stage4MaximumSize() +
// dng_render_stage4_output_size() pair, and the same non-Bayer downgrade of
// max_dim), without decoding: no ReadStage1Image, no Stage3, no Stage4, no
// pool acquire. Returns false on parse failure, with result.error_code set.
// On success result.width/height carry the extent and nothing else is touched.
bool dng_pipeline_probe_output_size(const char *file_path, int32_t max_dim,
                                    DngPipelineResult &result);

bool dng_pipeline_warmup_for_size(int32_t width, int32_t height);

// Test-only: widens race windows deterministically so the concurrency gate is
// near-deterministic rather than probabilistic. DNG_RACE_DELAY_US=0 (the
// default, including unset) makes this a predictable-branch no-op. This is NOT
// removed after the migration — it stays as a permanent debugging instrument.
void dngRaceDelay();

bool dng_pipeline_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing,
                                std::vector<uint16_t> *stage3_workspace = nullptr);
