// Phase 10 Sprint C — Stage 2 OpcodeList2 GPU bridge.
//
// Currently only handles dngOpcode_MapPolynomial (opcodeID = 8). The bridge
// is plugged into dng_opcode_list::Apply() before SDK opcode dispatch; if it
// returns true the caller MUST skip the SDK fallback path because the image
// has already been updated in-place by the Halide kernel.
//
// Stage 2 image is host-resident uint16 (dng_simple_image, ttShort). The
// kernel does the [0,1] normalize / Horner / [0,1]→uint16 round-trip itself,
// so no extra pre/post pass is required on the host side. Device handoff is
// left to Sprint D.

#pragma once

#include <cstdint>

// Forward declarations to avoid pulling DNG SDK headers into bridge clients.
class dng_host;
class dng_image;
class dng_opcode;

// Returns true if the opcode was handled on GPU (host MUST skip SDK Apply).
// Returns false to fall through to SDK fallback (unsupported opcode, GPU
// dispatch failure, env-disabled, plane / area edge-case, etc.).
bool halide_try_dispatch_opcode2(dng_host &host,
                                 dng_opcode &opcode,
                                 dng_image &image);
