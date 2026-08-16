#ifndef DNG_CFA_PHASE_H_
#define DNG_CFA_PHASE_H_

// ---------------------------------------------------------------------------
// CFA phase resolution (2026-08-16).
//
// The demosaic kernels used to assume red sits at (even row, even col), i.e.
// RGGB. That is only one of the four legal Bayer phases; a BGGR file decoded
// with the RGGB assumption comes out with R and B swapped (blue sky renders
// orange). The DNG spec requires the reader to honor CFAPattern, so read it.
//
// `red_x` / `red_y` are the column / row parity of the red CFA site (see the
// convention note in dng_mosaic_halide.h): RGGB=(0,0), GRBG=(1,0),
// GBRG=(0,1), BGGR=(1,1).
//
// CFAPattern's origin is the top-left of the ActiveArea rectangle, and the
// SDK's linearization step crops Stage2 to exactly that rectangle
// (dng_linearization_info.cpp:453, `dstTile = srcTile - fActiveArea.TL()`),
// so the pattern indices are already the Stage2 buffer parities — no extra
// offset is needed.
//
// Header-only on purpose: the resolver is needed by the production pipeline
// and by several standalone Stage3 test tools that do not link the pipeline
// translation unit, and a second hand-written copy of this mapping is exactly
// the failure mode this change exists to remove.
// ---------------------------------------------------------------------------

#include <dng_mosaic_info.h>
#include <dng_negative.h>
#include <dng_tag_values.h>

// Resolves the Bayer CFA phase from an already-parsed dng_mosaic_info.
//
// Split out from the dng_negative overload below purely so the mapping is
// testable without opening a DNG file: tests/test_cfa_phase.cpp feeds it a
// synthetic mosaic info for each of the four phases. GRBG and GBRG are the
// only phases that are not transpose-invariant, so they are what pins the
// row/col roles down.
//
// On success writes the red site's (column, row) parity and returns true.
// When the phase cannot be determined it writes the RGGB fallback (0, 0),
// sets `*reason_out` (when non-null) to a static explanation string and
// returns false — callers should log the reason but must not fail the decode:
// a plausible phase beats refusing to open the file.
inline bool dng_resolve_cfa_phase(const dng_mosaic_info *mosaic,
                                  int &red_x,
                                  int &red_y,
                                  const char **reason_out = nullptr) {
    red_x = 0;
    red_y = 0;

    if (!mosaic) {
        if (reason_out) *reason_out = "no MosaicInfo on negative";
        return false;
    }
    if (mosaic->fCFAPatternSize.h != 2 || mosaic->fCFAPatternSize.v != 2) {
        if (reason_out) *reason_out = "CFAPattern repeat is not 2x2";
        return false;
    }

    // fCFAPattern holds ColorKeyCode values (0=R, 1=G, 2=B), matching how the
    // SDK itself compares them against fCFAPlaneColor (dng_ifd.cpp:2646,
    // dng_mosaic_info.cpp:1162-1167).
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            if (mosaic->fCFAPattern[row][col] == colorKeyRed) {
                red_x = col;
                red_y = row;
                return true;
            }
        }
    }

    if (reason_out) *reason_out = "CFAPattern contains no red site";
    return false;
}

// Resolves the Bayer CFA phase of `negative` from its parsed CFAPattern.
// Same contract as the overload above.
inline bool dng_resolve_cfa_phase(const dng_negative &negative,
                                  int &red_x,
                                  int &red_y,
                                  const char **reason_out = nullptr) {
    return dng_resolve_cfa_phase(negative.GetMosaicInfo(), red_x, red_y,
                                 reason_out);
}

#endif  // DNG_CFA_PHASE_H_
