// Phase 10 Sprint C — Stage 2 OpcodeList2 GPU bridge.
//
// Currently only handles dngOpcode_MapPolynomial (opcodeID = 8). The bridge
// is plugged into dng_opcode_list::Apply() before SDK opcode dispatch; if it
// returns true the caller MUST skip the SDK fallback path because the image
// has already been updated in-place by the Halide kernel.
//
// Stage 2 image is normally copied back into host-resident uint16
// (dng_simple_image, ttShort). The production lossy pipeline can opt into a
// narrow device-resident handoff after the batched RGB MapPolynomial path.

#pragma once

#include <cstdint>

struct halide_buffer_t;

// Forward declarations to avoid pulling DNG SDK headers into bridge clients.
class dng_host;
class dng_image;
class dng_negative;
class dng_opcode;
class dng_opcode_list;

// Returns true if the opcode was handled on GPU (host MUST skip SDK Apply).
// Returns false to fall through to SDK handling only when the opcode shape is
// unsupported or the route is disabled. GPU dispatch failure sets the
// halide_stage2_ol2_dispatch_failed() flag and returns false; caller must check.
bool halide_try_dispatch_opcode2(dng_host &host,
                                 dng_opcode &opcode,
                                 dng_image &image);

// Returns the number of consecutive Stage2 opcodes handled on GPU. Currently
// returns 3 for the batched RGB MapPolynomial fast path, or 0 when the batch
// shape is unsupported. GPU dispatch failure sets the
// halide_stage2_ol2_dispatch_failed() flag and returns 0; caller must check.
uint32_t halide_try_dispatch_opcode2_batch(dng_host &host,
                                           dng_negative &negative,
                                           dng_opcode_list &list,
                                           uint32_t start_index,
                                           dng_image &image);

struct Stage2Opcode2DeviceHandoff {
    halide_buffer_t *device_buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t planes = 0;
    uint32_t pixel_range = 0;
};

// Pipeline-only guard: when enabled, the batched MapPolynomial path leaves its
// output device-resident and records a handoff buffer instead of immediately
// copying back into the SDK Stage2 image. Callers must either consume the
// device buffer or call halide_stage2_ol2_device_handoff_copy_to_host() before
// returning to any host-owned Stage3 path.
void halide_stage2_ol2_set_device_handoff_enabled(bool enabled);
void halide_stage2_ol2_clear_device_handoff();
bool halide_stage2_ol2_get_device_handoff(Stage2Opcode2DeviceHandoff &out);
bool halide_stage2_ol2_device_handoff_copy_to_host();

// M-6: Query/clear the OL2 GPU dispatch failure flag.  Replaces the old
// DngOl2DispatchError throw-as-control-flow path.  When a GPU dispatch fails,
// the dispatch functions set this flag and return normally (0 for batch, false
// for single); the caller (SDK opcode loop + pipeline) checks and acts on it.
bool halide_stage2_ol2_dispatch_failed();
void halide_stage2_ol2_clear_dispatch_failure();

// Lazy actual-size prewarm for the batched 3-plane polynomial kernel.
// Fires a dummy dispatch at (width x height) if that dimension pair has not
// been warmed yet in this process. Safe to call concurrently; uses an internal
// mutex-protected set keyed by (width << 32 | height). No-op when
// DNG_STAGE2_OL2_PREWARM=0 or DNG_STAGE2_OL2_HALIDE=0.
void halide_prewarm_polynomial3_for_size(int width, int height);
