#ifndef LIBRAW_GPU_INPUT_ADAPTER_H_
#define LIBRAW_GPU_INPUT_ADAPTER_H_

// The ONLY LibRaw -> RawGpuInput adapter in the tree (spec section 2.3.7).
//
// Field mapping, enum normalization and contract validation. Explicitly not
// allowed here: a second camera lookup, demosaic, tone, gamma, or a full-frame
// rotation (spec section 6.4.4). No branch on backend or camera vendor.

#include "libraw_frontend.h"
#include "raw_pipeline_contract.h"

// Maps a LibRaw CFA colour index through idata.cdesc ("RGBG", "GMCY", ...).
// An unrecognised descriptor character maps to kRawColorKeyUnknown - never to
// Green, because a coerced key silently mis-colours the image
// (spec section 3.3.5).
RawColorKey raw_color_key_from_libraw(uint32_t libraw_index, uint32_t colors,
                                      const char* cdesc);

// LibRaw sizes.flip -> EXIF orientation. Unknown values map to
// kRawOrientationUnknown, which the validator then rejects (spec section 4.1.8).
//
// flip is a dcraw bit-field, NOT an EXIF code: bit 2 transposes, bit 1 mirrors
// vertically, bit 0 mirrors horizontally. The inverse of LibRaw's own EXIF->flip
// table, third_party/libraw/src/metadata/tiff.cpp:631
// (`t_flip = "50132467"[get2() & 7] - '0'`, get2() being EXIF tag 0x0112), is
// therefore the authority here:
//   flip 0->EXIF 1  flip 1->EXIF 2  flip 2->EXIF 4  flip 3->EXIF 3
//   flip 4->EXIF 5  flip 5->EXIF 8  flip 6->EXIF 6  flip 7->EXIF 7
// Corroborated by third_party/libraw/src/metadata/identify.cpp:1294-1306
// (270 deg -> flip 5, 180 -> 3, 90 -> 6) and by composing the bit semantics.
// NOTE: this contradicts plan Task 7, which specifies flip 5 -> EXIF 5; that is
// a plan defect (round-4 finding F-R4-01), upheld deviation.
RawOrientation raw_orientation_from_libraw_flip(int32_t flip);

// LibRaw colour index (what FC returns) at a PLANE-RELATIVE coordinate.
//
// CONTRACT ORIGIN RULING (team-lead, round 5): RawLayoutDescriptor::cfa_pattern
// and RawGpuInput::black describe planes[0] starting at ITS OWN (0,0), so the
// descriptor is self-describing for the buffer the GPU layer consumes and does
// not require default_crop to be interpreted.
//
// LibRaw's `filters` word is NOT in that origin - it is VISIBLE-relative.
// Verified at three independent sites in the vendored tree:
//   1. third_party/libraw/src/preprocessing/raw2image.cpp:144-148 pairs
//      fcol(row, col) with raw_image[(row + top_margin) * pitch + col +
//      left_margin], so the row/col handed to FC are visible-origin.
//   2. third_party/libraw/src/utils/utils_dcraw.cpp:41-42 adds the margins back
//      for the filters==1 table (filter[(row+top_margin)&15][...]), which is only
//      coherent if the incoming row/col are visible-relative.
//   3. third_party/libraw/src/metadata/identify.cpp:2946-2949 rewrites `filters`
//      itself by XOR-ing (left_margin & 1) and (top_margin << 1) for a Panasonic
//      body - LibRaw normalising the word against margin parity, which only makes
//      sense in visible coordinates.
// So the colour at plane (r, c) is FC(r - top_margin, c - left_margin); since FC
// is 2-periodic and -x is congruent to x mod 2, that is FC((r + top_margin) & 1,
// (c + left_margin) & 1). With even margins the shift is a no-op, which is why no
// present corpus file (crop 0,0) can observe it.
//
// The X-Trans branch needs NO such shift and that is verified, not assumed:
// identify.cpp:2548-2551 builds the visible-relative `xtrans` FROM `xtrans_abs`
// by adding the margins, so `xtrans_abs` - the array the adapter reads - is
// already the absolute raw-plane tile.
uint32_t raw_bayer_channel_index_at_plane(uint32_t filters,
                                          uint32_t left_margin,
                                          uint32_t top_margin,
                                          uint32_t plane_row,
                                          uint32_t plane_col);

// PRECONDITION CHECK for the 2x2 Bayer branch (round-5 finding S3).
//
// The dcraw bit index used by raw_bayer_channel_index_at_plane addresses an
// 8-ROW by 2-column table (bit position (((row << 1) & 14) + (col & 1)) << 1,
// i.e. one nibble per row for rows 0..7), but the adapter reads only rows 0 and
// 1 and then declares cfa_repeat_height == 2. That is a PRECONDITION, not a
// property of the word, and the vendored tree contains real counter-examples:
//   third_party/libraw/src/metadata/identify.cpp:1987  0xe1e4e1e4 (PowerShot 600)
//   third_party/libraw/src/metadata/identify.cpp:1997  0x1e4e1e4e (PowerShot A5)
//   third_party/libraw/src/metadata/identify.cpp:2005  0x1b4e4b1e (PowerShot A50)
//   third_party/libraw/src/metadata/identify.cpp:2012  0x1e4b4e1b (PowerShot Pro70)
// all of which are genuinely 4-row periodic (0x1e4e1e4e decodes to row0=(2,3),
// row1=(0,1), row2=(2,3), row3=(1,0)), plus identify.cpp:2856 which sets
// filters == 1, selecting LibRaw's 16x16 table in
// third_party/libraw/src/utils/utils_dcraw.cpp:41-42 rather than a packed word.
//
// Those five are rejected today only INCIDENTALLY and downstream: identify.cpp
// :1278 gives them cdesc "GMCY" (colors != 3) or "RBTG" (identify.cpp:2857), so
// the colour keys fail raw_classify_layout's others == 0 / Unknown rules. That
// safety is real but unintentional; any future body with a three-colour cdesc
// and a >2-row-periodic word would silently get a wrong 2x2 phase on half its
// rows - the same "guessed phase silently mis-colours" failure that
// raw_gpu_pipeline.cpp refuses for a MISSING phase. This function makes the
// precondition explicit and fails loud instead.
//
// Returns kRawSuccess when rows 2..7 of the word agree with rows 0/1 by parity
// (so a 2x2 tile is a faithful summary), otherwise kRawErrLayoutUnsupported
// with a named reason in reason_out. filters == 1 is rejected by name.
// Not called for filters == 9 (X-Trans), which never enters the 2x2 branch.
RawErrorCode raw_bayer_filters_check_2x2(uint32_t filters, char* reason_out,
                                         size_t reason_cap);

// Folds LibRaw's THREE black-level terms into one repeating tile.
//
// After open_file() + unpack() LibRaw has not run adjust_bl(), so the effective
// black at a site is
//     color.black
//   + color.cblack[FC(row, col)]                                  <- per channel
//   + color.cblack[6 + (row % cblack[4]) * cblack[5] + col % cblack[5]]
// exactly as LibRaw subtracts it in
// third_party/libraw/src/preprocessing/subtract_black.cpp:31-51. adjust_bl()
// (third_party/libraw/src/utils/utils_libraw.cpp:468-549), which is what folds
// color.black into cblack[0..3], is reachable only from subtract_black.cpp:20,
// dcraw_process.cpp:85 and raw2image.cpp:428 - none of which the frontend calls.
// Dropping the middle term produces a per-channel colour cast (round-4 finding
// F-R4-02).
//
// Declared here rather than kept static because no corpus file has a nonzero
// cblack[0..3], so a synthetic unit test is the only coverage available (same
// reasoning as raw_frontend_pixels_live_in_raw_image).
//
// channel_index is the per-site LibRaw colour index tile (what FC returns),
// cfa_w x cfa_h row-major, and must ALREADY be in the plane origin (build it
// with raw_bayer_channel_index_at_plane, or from xtrans_abs). channel_black is
// cblack[0..3]. spatial_black is &cblack[6] with spatial_w == cblack[5] and
// spatial_h == cblack[4], row-major.
//
// left_margin / top_margin shift the SPATIAL term into the plane origin. That
// term is visible-relative for the same reason the CFA pattern is:
// subtract_black.cpp:38-51 walks q over imgdata.image, whose dimensions are
// S.iheight x S.iwidth, and open.cpp:356-357 sets iwidth/iheight from the
// VISIBLE width/height - so cblack[6 + ...] is indexed by visible row/col.
// The per-channel term needs no separate shift: it follows channel_index, which
// the caller has already placed in the plane origin.
//
// THE TWO SHIFTS HAVE DIFFERENT PERIODS - do not carry a parity assumption
// across from the CFA pattern. raw_bayer_channel_index_at_plane shifts by margin
// PARITY, because FC is 2-periodic. The spatial black term shifts by margin
// MODULO THE TILE DIMS (cblack[5] x cblack[4]). So "the margin is even, the
// shift is a no-op" is TRUE for cfa_pattern and FALSE in general here: top=2
// against a 3x3 spatial tile gives 2 % 3 == 2 and still rotates. Pinned by the
// test cases even-top-margin-still-shifts-3x3 and even-margin-is-noop-2x2, which
// use the same "even" margin and disagree on purpose.
//
// The emitted tile is the element-wise sum over lcm(cfa, spatial) dimensions.
// If that lcm exceeds the contract's 8x8 ceiling the function FAILS with
// kRawErrLayoutUnsupported rather than truncating to a wrong tile.
RawErrorCode raw_black_pattern_from_libraw(uint32_t black_scalar,
                                           const uint32_t* channel_black,
                                           const uint8_t* channel_index,
                                           uint32_t cfa_w, uint32_t cfa_h,
                                           const uint32_t* spatial_black,
                                           uint32_t spatial_w, uint32_t spatial_h,
                                           uint32_t left_margin, uint32_t top_margin,
                                           RawBlackLevelPattern* out,
                                           char* reason_out, size_t reason_cap);

// Row-major 3x3 inverse. Returns false (leaving out9 untouched) when any input
// is non-finite or the matrix is not invertible. Used to turn LibRaw's
// camera-from-XYZ cam_xyz into the camera-to-PCS direction the contract field
// names and Task 8 consumes - see docs/logs/2026-08-25/r5-camera-to-pcs-ruling.md.
bool raw_invert_3x3(const float in9[9], float out9[9]);

class LibRawGpuInputAdapter {
 public:
    LibRawGpuInputAdapter() = default;
    ~LibRawGpuInputAdapter() = default;
    LibRawGpuInputAdapter(const LibRawGpuInputAdapter&) = delete;
    LibRawGpuInputAdapter& operator=(const LibRawGpuInputAdapter&) = delete;

    // Fills *out_input / *out_develop and returns the result of
    // raw_validate_gpu_input, so no caller can skip validation.
    //
    // Ownership: this object owns the plane array and CFA pattern array that
    // *out_input points at. *this must outlive *out_input.
    RawErrorCode build(const LibRawFrontendContext& ctx,
                       RawGpuInput* out_input,
                       RawDevelopParams* out_develop,
                       char* reason_out,
                       size_t reason_cap);

 private:
    RawPlaneView planes_[1]{};
    RawColorKey cfa_pattern_[kRawMaxCfaPatternCount]{};
};

#endif  // LIBRAW_GPU_INPUT_ADAPTER_H_
