// LibRawGpuInputAdapter coverage.
//
// Two properties carry the architecture:
//   1. X-Trans keeps the full 6x6 pattern (a filters==9 collapse loses it).
//   2. The contract is backend-invariant: RawSpeed3 and forced-native unpacks
//      of the SAME file must yield identical layout/rect/orientation/black/
//      white/matrix bytes. Only pixels may differ.
//
// Usage: --manifest <path>. Missing corpus files are SKIPped by name.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "libraw_frontend.h"
#include "libraw_gpu_input_adapter.h"
#include "raw_contract_validate.h"

// Declared in src/pipeline/libraw_gpu_input_adapter.cpp (this test compiles that
// TU directly, cmake/tests.cmake:746-752). They are NOT in
// libraw_gpu_input_adapter.h because that header is outside the Stage 1 file
// ownership boundary.
//
// The route values mirror that TU's RawCameraMatrixRoute enum: 0 none,
// 1 rgb_cam (primary), 2 cam_xyz (fallback). checkRouteTable() drives all three
// and asserts the returned value, so a renumbering there fails here rather than
// silently mislabelling a route.
extern "C" void raw_srgb_to_pcs_matrix(float out9[9]);
extern "C" void raw_pcs_white(float out3[3]);
int raw_camera_to_pcs_from_libraw(const float* rgb_cam, const float* cam_xyz,
                                  uint32_t raw_color, uint32_t colors,
                                  RawColorTransform* out, char* reason_out,
                                  size_t reason_cap);
// Stage 2 (design section 2.2) white-balance source-selection chain. Mirrored
// from src/pipeline/libraw_gpu_input_adapter.cpp for the same reason as the
// route function above: no corpus file reaches the pre_mul / identity /
// as_shot_wb_applied guards.
void raw_white_balance_from_libraw(const float* cam_mul, const float* pre_mul,
                                   uint32_t as_shot_wb_applied, uint32_t colors,
                                   float out_neutral[4], char* reason_out,
                                   size_t reason_cap);

namespace {

int failures = 0;
int checked = 0;

void report(const char* name, const char* id, bool ok, const char* detail) {
    std::printf("[LibRawAdapter] %s%s%s %s -> %s\n", id ? id : "", id ? " " : "",
                name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

struct Sample { std::string id, path, expect_backend, expect_error, expect_layout; };

std::string field(const std::string& obj, const char* key) {
    const std::string needle = std::string("\"") + key + "\": \"";
    const size_t at = obj.find(needle);
    if (at == std::string::npos) return "";
    const size_t start = at + needle.size();
    const size_t end = obj.find('"', start);
    return end == std::string::npos ? "" : obj.substr(start, end - start);
}

std::vector<Sample> loadManifest(const char* path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Sample> out;
    size_t pos = 0;
    while ((pos = text.find('{', pos)) != std::string::npos) {
        const size_t end = text.find('}', pos);
        if (end == std::string::npos) break;
        const std::string obj = text.substr(pos, end - pos);
        Sample s{field(obj, "id"), field(obj, "path"), field(obj, "expect_backend"),
                 field(obj, "expect_error"), field(obj, "expect_layout")};
        if (!s.id.empty() && !s.path.empty()) out.push_back(s);
        pos = end + 1;
    }
    return out;
}

bool fileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

// Round 2 Task 2.1 (diagnostic only): dump-only case. Iterates the FULL
// manifest, including malformed_* ids that the assertion loop below skips by
// name, and prints the camera-matrix route raw_camera_to_pcs_from_libraw
// actually chose for every entry present on disk. Calls that function
// directly (same one AC-1.2's checkRouteTable() pins with synthetic data) so
// the printed route is not re-derived from a second predicate. No
// assertions -- this function cannot fail the run; native/scripts/tmp/
// captures its stdout into route_census.md together with RC=$? and the HEAD
// hash at capture time.
void dumpRouteCensus(const std::vector<Sample>& samples) {
    for (const Sample& s : samples) {
        if (!fileExists(s.path)) {
            std::printf("[RouteCensus] %s SKIP-missing-file\n", s.id.c_str());
            continue;
        }
        LibRawFrontendContext ctx;
        if (ctx.open_and_unpack(s.path.c_str()) != kRawSuccess) {
            std::printf("[RouteCensus] %s ROUTE=open-failed\n", s.id.c_str());
            continue;
        }
        const LibRawRawView& v = ctx.raw_view();
        RawColorTransform xf{};
        char reason[256] = {0};
        const int route = raw_camera_to_pcs_from_libraw(
            v.rgb_cam, v.cam_xyz, v.raw_color, v.colors, &xf, reason, sizeof(reason));
        const char* label =
            route == 1 ? "rgb_cam" : route == 2 ? "cam_xyz" : "none";
        std::printf(
            "[RouteCensus] %s ROUTE=%s raw_color=%u colors=%u reason=\"%s\"\n",
            s.id.c_str(), label, v.raw_color, v.colors, reason);
    }
}

// Everything in RawGpuInput except the borrowed pixel pointer: this is what
// must not move when the unpack backend changes.
struct MetaFingerprint {
    RawLayoutDescriptor layout;
    RawColorKey pattern[kRawMaxCfaPatternCount];
    RawRect active, crop;
    RawOrientation orientation;
    RawBlackLevelPattern black;
    float white[4];
    float wb[4];
    RawColorTransform matrix;
    uint32_t width, height;
};

MetaFingerprint fingerprint(const RawGpuInput& in) {
    // memset, not value-init: the comparison below is a raw memcmp, so the
    // padding bytes have to be defined too.
    MetaFingerprint f;
    std::memset(&f, 0, sizeof(f));
    f.layout = in.layout;
    f.layout.cfa_pattern = nullptr;   // pointer identity is not part of the contract
    if (in.layout.cfa_pattern) {
        std::memcpy(f.pattern, in.layout.cfa_pattern,
                    in.layout.cfa_pattern_count * sizeof(RawColorKey));
    }
    f.active = in.active_area;
    f.crop = in.default_crop;
    f.orientation = in.orientation;
    f.black = in.black;
    std::memcpy(f.white, in.white_level, sizeof(f.white));
    std::memcpy(f.wb, in.as_shot_neutral, sizeof(f.wb));
    f.matrix = in.camera_to_pcs;
    if (in.planes && in.plane_count) {
        f.width = in.planes[0].width;
        f.height = in.planes[0].height;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Round-5 pre-req cases (F-R4-01, F-R4-02, camera_to_pcs direction).
//
// None of these can be driven from the corpus: every present file has flip == 0,
// cblack[0..3] == {0,0,0,0}, so the synthetic path below is the only coverage.
// ---------------------------------------------------------------------------

// F-R4-01. Expected table is the inverse of LibRaw's own EXIF->flip string,
// third_party/libraw/src/metadata/tiff.cpp:631 ("50132467"[exif & 7] - '0'),
// re-derived here rather than copied from the adapter so the two can disagree.
void checkFlipTable() {
    struct Row { int32_t flip; RawOrientation want; const char* why; };
    static const Row rows[] = {
        {0, kRawOrientationTopLeft,     "EXIF 1 identity"},
        {1, kRawOrientationTopRight,    "EXIF 2 mirror horizontal"},
        {2, kRawOrientationBottomLeft,  "EXIF 4 mirror vertical"},
        {3, kRawOrientationBottomRight, "EXIF 3 rotate 180"},
        {4, kRawOrientationLeftTop,     "EXIF 5 transpose"},
        {5, kRawOrientationLeftBottom,  "EXIF 8 rotate 270 CW"},
        {6, kRawOrientationRightTop,    "EXIF 6 rotate 90 CW"},
        {7, kRawOrientationRightBottom, "EXIF 7 anti-transpose"},
    };
    // Independent re-derivation: invert the LibRaw table at test time.
    static const char kExifToFlip[] = "50132467";
    for (const Row& r : rows) {
        int derived_exif = -1;
        for (int exif = 1; exif <= 8; ++exif) {
            if (kExifToFlip[exif & 7] - '0' == r.flip) derived_exif = exif;
        }
        char detail[192];
        const RawOrientation got = raw_orientation_from_libraw_flip(r.flip);
        std::snprintf(detail, sizeof(detail),
                      "flip=%d libraw-table-exif=%d want=%d got=%d (%s)", r.flip,
                      derived_exif, static_cast<int>(r.want), static_cast<int>(got),
                      r.why);
        report("flip-mapping", nullptr,
               got == r.want && derived_exif == static_cast<int>(r.want), detail);
    }
    char detail[128];
    const RawOrientation got = raw_orientation_from_libraw_flip(9);
    std::snprintf(detail, sizeof(detail), "flip=9 -> %d (want Unknown)",
                  static_cast<int>(got));
    report("flip-out-of-range", nullptr, got == kRawOrientationUnknown, detail);
}

// F-R4-02. LibRaw's own effective black at a site after open_file()+unpack(),
// transcribed straight from third_party/libraw/src/preprocessing/
// subtract_black.cpp:31-51 (adjust_bl() has NOT run, so color.black is still a
// separate term). Deliberately NOT written in terms of the adapter's helper.
// row/col are PLANE coordinates; LibRaw's spatial tile is indexed in VISIBLE
// coordinates (subtract_black.cpp:38-51 walks imgdata.image, sized iheight x
// iwidth, and open.cpp:356-357 sets those from the visible width/height), so the
// margins are subtracted here to get back to LibRaw's own index.
uint32_t librawEffectiveBlack(uint32_t scalar, const uint32_t cb[4],
                              const uint8_t* cfa, uint32_t cfa_w, uint32_t cfa_h,
                              const uint32_t* sp, uint32_t sp_w, uint32_t sp_h,
                              uint32_t left_margin, uint32_t top_margin,
                              uint32_t row, uint32_t col) {
    uint32_t v = scalar;
    if (cfa && cfa_w && cfa_h) v += cb[cfa[(row % cfa_h) * cfa_w + (col % cfa_w)]];
    if (sp && sp_w && sp_h) {
        const uint32_t sr = (row + sp_h - (top_margin % sp_h)) % sp_h;
        const uint32_t sc = (col + sp_w - (left_margin % sp_w)) % sp_w;
        v += sp[sr * sp_w + sc];
    }
    return v;
}

// Emitted tile is a repeating pattern: read it the way a consumer must.
float tileAt(const RawBlackLevelPattern& b, uint32_t row, uint32_t col) {
    const uint32_t w = b.repeat_width ? b.repeat_width : 1;
    const uint32_t h = b.repeat_height ? b.repeat_height : 1;
    return b.values[(row % h) * w + (col % w)];
}

void checkOneBlackCase(const char* name, uint32_t scalar, const uint32_t cb[4],
                       const uint8_t* cfa, uint32_t cfa_w, uint32_t cfa_h,
                       const uint32_t* sp, uint32_t sp_w, uint32_t sp_h,
                       uint32_t left_margin, uint32_t top_margin,
                       uint32_t want_tile_w, uint32_t want_tile_h) {
    RawBlackLevelPattern black{};
    char reason[256] = {0};
    const RawErrorCode rc = raw_black_pattern_from_libraw(
        scalar, cb, cfa, cfa_w, cfa_h, sp, sp_w, sp_h, left_margin, top_margin,
        &black, reason, sizeof(reason));

    const bool dims_ok = (rc == kRawSuccess) && black.repeat_width == want_tile_w &&
                         black.repeat_height == want_tile_h;
    bool ok = dims_ok;
    bool value_mismatch = false;
    uint32_t bad_row = 0, bad_col = 0;
    float bad_got = 0.0f, bad_want = 0.0f;
    if (ok) {
        // Sweep a region larger than every period involved so a wrong tile size
        // or a wrong phase cannot hide.
        for (uint32_t r = 0; ok && r < 24; ++r) {
            for (uint32_t c = 0; c < 24; ++c) {
                const float want = static_cast<float>(librawEffectiveBlack(
                    scalar, cb, cfa, cfa_w, cfa_h, sp, sp_w, sp_h, left_margin,
                    top_margin, r, c));
                const float got = tileAt(black, r, c);
                if (got != want) {
                    ok = false;
                    value_mismatch = true;
                    bad_row = r; bad_col = c; bad_got = got; bad_want = want;
                    break;
                }
            }
        }
    }
    char detail[320];
    if (value_mismatch) {
        std::snprintf(detail, sizeof(detail),
                      "rc=%s tile=%ux%u want=%ux%u first VALUE mismatch at (%u,%u) "
                      "got=%.1f want=%.1f", raw_error_name(rc), black.repeat_width,
                      black.repeat_height, want_tile_w, want_tile_h, bad_row,
                      bad_col, bad_got, bad_want);
    } else {
        std::snprintf(detail, sizeof(detail), "rc=%s tile=%ux%u want=%ux%u%s",
                      raw_error_name(rc), black.repeat_width, black.repeat_height,
                      want_tile_w, want_tile_h,
                      dims_ok ? "" : " (TILE SIZE mismatch)");
    }
    report(name, "cblack", ok, detail);
}

void checkBlackFolding() {
    // LibRaw FC indices for an RGGB frame with cdesc "RGBG": R=0, G=1, B=2, G2=3.
    static const uint8_t kRggb[4] = {0, 1, 3, 2};
    static const uint32_t kChan[4] = {10, 20, 30, 40};
    static const uint32_t kZeroChan[4] = {0, 0, 0, 0};

    // 1. per-channel only. This is the case the pre-fix adapter got wrong: it
    //    emitted a flat 1x1 tile of `scalar`.
    checkOneBlackCase("channel-only", 100, kChan, kRggb, 2, 2, nullptr, 0, 0, 0, 0, 2, 2);

    // 2. per-channel + 2x2 spatial: both terms, same period.
    static const uint32_t kSp2x2[4] = {1, 2, 3, 4};
    checkOneBlackCase("channel-plus-spatial-2x2", 100, kChan, kRggb, 2, 2, kSp2x2,
                      2, 2, 0, 0, 2, 2);

    // 3. co-prime periods: 2x2 CFA against a 3x3 spatial tile -> lcm 6x6.
    static const uint32_t kSp3x3[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    checkOneBlackCase("channel-2x2-spatial-3x3-lcm", 50, kChan, kRggb, 2, 2, kSp3x3,
                      3, 3, 0, 0, 6, 6);

    // 4. X-Trans 6x6 channel tile against a 2x2 spatial tile -> lcm 6x6, which
    //    is 36 entries and still inside the 64-entry values[] array.
    static uint8_t xtrans[36];
    for (int i = 0; i < 36; ++i) xtrans[i] = static_cast<uint8_t>(i % 3);
    checkOneBlackCase("xtrans-6x6-spatial-2x2", 512, kChan, xtrans, 6, 6, kSp2x2,
                      2, 2, 0, 0, 6, 6);

    // 5. all-zero cblack[0..3] must NOT enlarge the tile: this is what keeps the
    //    present corpus byte-identical to round 4.
    checkOneBlackCase("zero-channel-stays-1x1", 800, kZeroChan, kRggb, 2, 2,
                      nullptr, 0, 0, 0, 0, 1, 1);
    checkOneBlackCase("zero-channel-keeps-spatial", 800, kZeroChan, kRggb, 2, 2,
                      kSp2x2, 2, 2, 0, 0, 2, 2);

    // 6. a combined period that does not fit must FAIL LOUD, never truncate.
    static const uint32_t kSp4x4[16] = {0};
    RawBlackLevelPattern black{};
    char reason[256] = {0};
    const RawErrorCode rc = raw_black_pattern_from_libraw(
        0, kChan, xtrans, 6, 6, kSp4x4, 4, 4, 0, 0, &black, reason, sizeof(reason));
    char detail[320];
    std::snprintf(detail, sizeof(detail), "6x6 CFA vs 4x4 spatial -> rc=%s (%s)",
                  raw_error_name(rc), reason);
    report("oversized-lcm-rejected", "cblack", rc == kRawErrLayoutUnsupported,
           detail);

    // 7. MARGINS. LibRaw indexes the spatial tile in VISIBLE coordinates; the
    //    contract tile is PLANE-relative, so a margin rotates it. No corpus file
    //    can reach any of this (all present samples crop at 0,0).
    checkOneBlackCase("odd-margin-spatial-2x2", 100, kZeroChan, kRggb, 2, 2,
                      kSp2x2, 2, 2, /*left*/ 1, /*top*/ 1, 2, 2);
    checkOneBlackCase("odd-margin-spatial-3x3", 0, kZeroChan, kRggb, 2, 2, kSp3x3,
                      3, 3, /*left*/ 5, /*top*/ 0, 3, 3);

    // 8. The distinction that a reader will otherwise get wrong: the CFA shift is
    //    PARITY-only, but the black spatial shift is MODULO THE TILE DIMS. So an
    //    EVEN margin is NOT automatically a no-op for black -- top=2 against a 3x3
    //    tile gives 2 % 3 == 2 and still rotates. This case exists to make that
    //    discoverable by name rather than incidental to another fixture.
    checkOneBlackCase("even-top-margin-still-shifts-3x3", 0, kZeroChan, kRggb, 2, 2,
                      kSp3x3, 3, 3, /*left*/ 0, /*top*/ 2, 3, 3);

    // 9. ...whereas for a 2x2 tile an even margin IS a no-op. This is the case
    //    the whole present corpus sits on, so if it ever shifts, recorded hashes
    //    move. Contrast with case 8 deliberately: same "even" margin, opposite
    //    outcome, because the period differs.
    checkOneBlackCase("even-margin-is-noop-2x2", 100, kChan, kRggb, 2, 2, kSp2x2,
                      2, 2, /*left*/ 4, /*top*/ 6, 2, 2);
}

// The CFA-origin half of the same ruling.
void checkBayerPlaneOrigin() {
    // RGGB as LibRaw encodes it: FC gives 0,1,1,2 over the 2x2, i.e. R G / G B
    // once cdesc "RGBG" is applied.
    const uint32_t kRggbFilters = 0x94949494u;

    struct Row { uint32_t left, top; uint32_t want[4]; const char* why; };
    static const Row rows[] = {
        {0, 0, {0, 1, 1, 2}, "no margin: plane == visible"},
        {1, 0, {1, 0, 2, 1}, "odd left: columns swap"},
        {0, 1, {1, 2, 0, 1}, "odd top: rows swap"},
        {1, 1, {2, 1, 1, 0}, "both odd: diagonal swap"},
        {2, 4, {0, 1, 1, 2}, "even margins: no-op"},
        {13, 7, {2, 1, 1, 0}, "large odd margins: parity only"},
    };
    for (const Row& r : rows) {
        uint32_t got[4];
        for (uint32_t row = 0; row < 2; ++row) {
            for (uint32_t col = 0; col < 2; ++col) {
                got[row * 2 + col] = raw_bayer_channel_index_at_plane(
                    kRggbFilters, r.left, r.top, row, col);
            }
        }
        // Independent oracle: LibRaw's own FC on the visible coordinate that
        // this plane site maps to. Not a copy of the adapter's expression.
        bool ok = true;
        for (uint32_t row = 0; row < 2 && ok; ++row) {
            for (uint32_t col = 0; col < 2; ++col) {
                const int vr = static_cast<int>(row) - static_cast<int>(r.top);
                const int vc = static_cast<int>(col) - static_cast<int>(r.left);
                const int vrp = ((vr % 2) + 2) % 2;
                const int vcp = ((vc % 2) + 2) % 2;
                const uint32_t oracle =
                    (kRggbFilters >> ((((vrp << 1) & 14) + (vcp & 1)) << 1)) & 3u;
                if (got[row * 2 + col] != oracle ||
                    got[row * 2 + col] != r.want[row * 2 + col]) {
                    ok = false;
                    break;
                }
            }
        }
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "margins l=%u t=%u -> {%u,%u,%u,%u} want {%u,%u,%u,%u} (%s)",
                      r.left, r.top, got[0], got[1], got[2], got[3], r.want[0],
                      r.want[1], r.want[2], r.want[3], r.why);
        report("bayer-plane-origin", "cfa", ok, detail);
    }
}

// S3 (round-5 review): the 2x2 Bayer branch summarises an 8-row dcraw word with
// two rows, which is only faithful for a genuinely 2-row-periodic word. The
// vendored tree holds four 4-row-periodic words plus filters==1 (a 16x16 table),
// which today are rejected only incidentally by the colour-descriptor rules.
// No corpus file carries any of them, so this synthetic case is the only
// coverage - same situation as the flip table and the cblack folding above.
void checkFiltersPeriodicity() {
    struct Row { uint32_t filters; bool want_ok; const char* why; };
    static const Row rows[] = {
        {0x94949494u, true,  "RGGB, 2-row periodic"},
        {0x61616161u, true,  "BGGR, 2-row periodic"},
        {0x49494949u, true,  "GRBG, 2-row periodic"},
        {0x16161616u, true,  "GBRG, 2-row periodic"},
        {0x9c9c9c9cu, true,  "RGBE (DSC-F828) IS 2-row periodic; the fourth "
                             "colour is S4's problem, not this guard's"},
        {0xe1e4e1e4u, false, "PowerShot 600, identify.cpp:1987, 4-row periodic"},
        {0x1e4e1e4eu, false, "PowerShot A5, identify.cpp:1997, 4-row periodic"},
        {0x1b4e4b1eu, false, "PowerShot A50, identify.cpp:2005, 4-row periodic"},
        {0x1e4b4e1bu, false, "PowerShot Pro70, identify.cpp:2012, 4-row periodic"},
        {1u,          false, "filters==1, identify.cpp:2856, 16x16 table"},
    };
    for (const Row& r : rows) {
        char reason[256] = {0};
        const RawErrorCode rc =
            raw_bayer_filters_check_2x2(r.filters, reason, sizeof(reason));

        // Independent oracle: decode the word with dcraw's own bit expression
        // (transcribed here, not called through the adapter helper) and ask
        // whether rows 0..7 really do repeat with period 2. filters==1 is a
        // table selector rather than a packed word, so it is excluded by name
        // in the oracle exactly as the implementation excludes it.
        bool oracle_ok = (r.filters != 1u);
        for (int row = 2; row < 8 && oracle_ok; ++row) {
            for (int col = 0; col < 2; ++col) {
                const uint32_t got =
                    (r.filters >> ((((row << 1) & 14) + (col & 1)) << 1)) & 3u;
                const uint32_t want =
                    (r.filters >> (((((row & 1) << 1) & 14) + (col & 1)) << 1)) & 3u;
                if (got != want) { oracle_ok = false; break; }
            }
        }

        const bool got_ok = (rc == kRawSuccess);
        // A rejection must also NAME itself: a silent kRawErrLayoutUnsupported
        // with an empty reason is the failure mode this finding is about.
        const bool reason_ok = got_ok ? true : (reason[0] != '\0');
        char detail[384];
        std::snprintf(detail, sizeof(detail),
                      "filters=0x%08x rc=%s want=%s oracle=%s reason=\"%s\" (%s)",
                      r.filters, raw_error_name(rc), r.want_ok ? "ok" : "reject",
                      oracle_ok ? "ok" : "reject", reason, r.why);
        report("filters-2x2-periodicity", "cfa",
               got_ok == r.want_ok && oracle_ok == r.want_ok && reason_ok &&
                   (got_ok || rc == kRawErrLayoutUnsupported),
               detail);
    }
}

// camera_to_pcs direction. See docs/logs/2026-08-25/r5-camera-to-pcs-ruling.md.
bool nearIdentity3x3(const float m[9], float tol) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float want = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(m[r * 3 + c] - want) > tol) return false;
        }
    }
    return true;
}

void mul3x3(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a[r * 3 + k] * b[k * 3 + c];
            out[r * 3 + c] = s;
        }
    }
}

void checkMatrixInverse() {
    // A real LibRaw cam_xyz (Sony ILCE-7M3, adobe_coeff table) scaled to float.
    static const float kCamXyz[9] = {
        0.7374f, -0.2389f, -0.0022f,
        -0.5140f, 1.3399f,  0.1976f,
        -0.1153f,  0.1607f, 0.5804f};
    float inv[9] = {0};
    const bool ok = raw_invert_3x3(kCamXyz, inv);
    float prod[9] = {0};
    if (ok) mul3x3(kCamXyz, inv, prod);
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "invertible=%d M*Minv diag=(%.6f,%.6f,%.6f)", ok ? 1 : 0,
                  prod[0], prod[4], prod[8]);
    report("invert-3x3-roundtrip", "matrix", ok && nearIdentity3x3(prod, 1e-4f),
           detail);

    static const float kSingular[9] = {1, 2, 3, 2, 4, 6, 7, 8, 9};
    float unused[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    const bool rejected = !raw_invert_3x3(kSingular, unused);
    std::snprintf(detail, sizeof(detail),
                  "singular rejected=%d out untouched=%d", rejected ? 1 : 0,
                  unused[0] == -1.0f ? 1 : 0);
    report("invert-3x3-singular", "matrix", rejected && unused[0] == -1.0f,
           detail);
}

// AC-1.2, on every real file: the D50-white invariant. camera_to_pcs must map a
// neutral (white-balanced) camera triple onto the PCS white.
//
// This REPLACES the previous "matrix-direction" case, which asserted
// cam_xyz * emitted == I. That case encoded the OLD contract, where camera_to_pcs
// was a bare Invert(cam_xyz) with no white handling; under the section-1.5 route
// table it is wrong on both routes (the rgb_cam route does not go through cam_xyz
// at all, and the cam_xyz route is now rescaled onto the PCS white). Observed
// RED before this rewrite: all 6 corpus samples failed it, e.g. local_sony_bayer
// "cam_xyz * emitted diag=(0.424245,1.069167,0.589579)"
// (tmp/verify/stage1_post1_run.txt:85).
//
// The tolerance is against raw_pcs_white(), i.e. the row sums of the SDK's own
// sRGB->PCS matrix, NOT the design document's literal (0.9642, 1.0, 0.8249):
// that literal is a 4-decimal rounding of D50 and sits 2.0e-4 from this SDK's
// PCStoXYZ() in Z, twice AC-1.2's own 1e-4 bound, so asserting against it would
// fail a correct implementation. Flagged to the lead 2026-08-28.
void checkPcsWhiteInvariant(const char* id, const RawColorTransform& xf,
                            const char* route_label) {
    if (!xf.valid) {
        std::printf("[LibRawAdapter] SKIP pcs-white-invariant %s (no matrix)\n", id);
        return;
    }
    float want[3];
    raw_pcs_white(want);
    float got[3];
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r) {
        got[r] = xf.m[r * 3 + 0] + xf.m[r * 3 + 1] + xf.m[r * 3 + 2];
        worst = std::fmax(worst, std::fabs(got[r] - want[r]));
    }
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "route=%s M*(1,1,1)=(%.6f,%.6f,%.6f) want=(%.6f,%.6f,%.6f) "
                  "max_abs_diff=%.8f tol=0.0001",
                  route_label, got[0], got[1], got[2], want[0], want[1], want[2],
                  worst);
    report("pcs-white-invariant", id, worst < 1e-4f, detail);
}

// AC-1.2's "both routes" half. No corpus file reaches the FALLBACK branch --
// every sample LibRaw recognises yields a usable rgb_cam -- so a corpus-only
// test would leave half the route table unexercised while reporting green
// (the 2026-07-10 allowlist lesson). These cases drive the branches directly.
void checkRouteTable() {
    char detail[320];
    char reason[256];

    // A plausible non-identity sRGB-from-camera matrix in LibRaw's [3][4]
    // layout. Column 3 is deliberately POISONED with huge values: rgb_cam has a
    // row stride of 4, and a stride-3 misread would pick those up. Rows are
    // white-preserving (each sums to 1) exactly as cam_xyz_coeff leaves them.
    const float kRgbCam[12] = {
         1.7f, -0.6f, -0.1f,  9999.0f,
        -0.2f,  1.5f, -0.3f, -9999.0f,
         0.05f, -0.45f, 1.4f,  1234.5f};
    // A plausible camera-from-XYZ in LibRaw's [4][3] layout (Sony-like).
    const float kCamXyz[12] = {
         0.7688f, -0.2199f, -0.0724f,
        -0.3129f,  1.0781f,  0.2588f,
        -0.0281f,  0.1287f,  0.6797f,
         0.0f,     0.0f,     0.0f};
    const float kIdentityRgbCam[12] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};

    // (a) PRIMARY: rgb_cam usable.
    {
        RawColorTransform xf{};
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            kRgbCam, kCamXyz, /*raw_color=*/0, /*colors=*/3, &xf, reason,
            sizeof(reason));
        std::snprintf(detail, sizeof(detail), "route=%d want=1 valid=%u",
                      route, xf.valid);
        report("route-primary-rgb_cam", "synthetic",
               route == 1 && xf.valid == 1, detail);
        checkPcsWhiteInvariant("synthetic-primary", xf, "rgb_cam");

        // Stride guard: the poisoned 4th column must not appear in the result.
        // 9999 * any sRGB->PCS coefficient would be >100; every legitimate
        // entry here is order 1.
        bool sane = true;
        for (int i = 0; i < 9; ++i) if (std::fabs(xf.m[i]) > 10.0f) sane = false;
        std::snprintf(detail, sizeof(detail),
                      "no |m[i]|>10 despite rgb_cam column 3 = "
                      "{9999,-9999,1234.5}; m[0]=%.6f", xf.m[0]);
        report("route-primary-row-stride-is-4", "synthetic", sane, detail);
    }

    // (b) FALLBACK: rgb_cam absent -> cam_xyz, rescaled onto the PCS white.
    {
        RawColorTransform xf{};
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            nullptr, kCamXyz, /*raw_color=*/0, /*colors=*/3, &xf, reason,
            sizeof(reason));
        std::snprintf(detail, sizeof(detail), "route=%d want=2 valid=%u",
                      route, xf.valid);
        report("route-fallback-cam_xyz", "synthetic",
               route == 2 && xf.valid == 1, detail);
        checkPcsWhiteInvariant("synthetic-fallback", xf, "cam_xyz");
    }

    // (c) rgb_cam present but LibRaw matched no camera (raw_color != 0): the
    //     array is an untrustworthy leftover, so the fallback must be taken.
    {
        RawColorTransform xf{};
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            kRgbCam, kCamXyz, /*raw_color=*/1, /*colors=*/3, &xf, reason,
            sizeof(reason));
        std::snprintf(detail, sizeof(detail), "route=%d want=2 (raw_color=1)",
                      route);
        report("route-raw_color-forces-fallback", "synthetic", route == 2, detail);
    }

    // (d) rgb_cam is the identity: also an untrustworthy leftover, not a
    //     legitimate "camera is already sRGB" claim (spec 4.1.9).
    {
        RawColorTransform xf{};
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            kIdentityRgbCam, kCamXyz, /*raw_color=*/0, /*colors=*/3, &xf, reason,
            sizeof(reason));
        std::snprintf(detail, sizeof(detail), "route=%d want=2 (rgb_cam==I)",
                      route);
        report("route-identity-rgb_cam-forces-fallback", "synthetic", route == 2,
               detail);
    }

    // (e) AC-2.3. colors == 4 (CMYG / RGBE / Quattro): rgb_cam is genuinely 3x4
    //     and cam_xyz would also yield a colour-wrong render, so the matrix must
    //     fail explicitly -- valid == 0, route None, and a reason naming the
    //     unsupported case ("4-colour"). This REPLACES the Stage 1 pin, which
    //     only asserted route != 1 (it fell through to the cam_xyz fallback and
    //     reported valid == 1). Observed RED against the pre-Stage-2 adapter:
    //     that build returned route=2 valid=1 (cam_xyz fallback), see
    //     tmp/verify/stage2_red_adapter.txt.
    {
        RawColorTransform xf{};
        for (int i = 0; i < 9; ++i) xf.m[i] = -7.0f;
        xf.valid = 1;
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            kRgbCam, kCamXyz, /*raw_color=*/0, /*colors=*/4, &xf, reason,
            sizeof(reason));
        const bool reason_has_4colour = std::strstr(reason, "4-colour") != nullptr;
        std::snprintf(detail, sizeof(detail),
                      "route=%d want=0 valid=%u want=0 reason=\"%s\" has_4colour=%d",
                      route, xf.valid, reason, reason_has_4colour ? 1 : 0);
        report("route-colors4-unsupported", "synthetic",
               route == 0 && xf.valid == 0 && reason_has_4colour, detail);
    }

    // (f) NEITHER: clean failure with a reason. Never an invented identity.
    {
        RawColorTransform xf{};
        for (int i = 0; i < 9; ++i) xf.m[i] = -7.0f;
        xf.valid = 1;
        reason[0] = '\0';
        const int route = raw_camera_to_pcs_from_libraw(
            nullptr, nullptr, /*raw_color=*/1, /*colors=*/3, &xf, reason,
            sizeof(reason));
        std::snprintf(detail, sizeof(detail),
                      "route=%d want=0 valid=%u want=0 reason=\"%s\"", route,
                      xf.valid, reason);
        report("route-none-clean-failure", "synthetic",
               route == 0 && xf.valid == 0 && reason[0] != '\0', detail);
    }
}

// Stage 2 white-balance source-selection chain (design section 2.2, AC-2.1,
// AC-2.2, AC-2.4). out_neutral[c] is the DNG-style as_shot_neutral = m[1]/m[c]
// over the selected multiplier vector m. None of these guards is reachable from
// the corpus (every present file has usable cam_mul and as_shot_wb_applied==0),
// so these synthetic cases are the only coverage.
void checkWhiteBalanceChain() {
    char detail[320];

    auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };

    // AC-2.1 a: as_shot_wb_applied == 1 forces neutral (1,1,1,1) even when a
    // perfectly usable cam_mul is present (re-applying it double-applies WB).
    // RED against the stub, which ignores as_shot_wb_applied and returns cam_mul
    // -> (0.5,1,0.25,1).
    {
        const float cam_mul[4] = {2.0f, 1.0f, 4.0f, 1.0f};
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(cam_mul, nullptr, /*as_shot_wb_applied=*/1,
                                      /*colors=*/3, n, reason, sizeof(reason));
        const bool ok = approx(n[0], 1) && approx(n[1], 1) && approx(n[2], 1) &&
                        approx(n[3], 1);
        std::snprintf(detail, sizeof(detail),
                      "as_shot_wb_applied=1 -> neutral=(%.3f,%.3f,%.3f,%.3f) want=(1,1,1,1)",
                      n[0], n[1], n[2], n[3]);
        report("wb-as-shot-forces-identity", "synthetic", ok, detail);
    }

    // AC-2.1 b: legacy auto-WB marker cam_mul[0] < -0.5 -> treated as absent,
    // pre_mul used. (Already handled by the stub's cam_mul[0] > 0 predicate; this
    // pins it against the new [0]/[2] predicate too.)
    {
        const float cam_mul[4] = {-1.0f, 1.0f, 1.0f, 1.0f};
        const float pre_mul[4] = {2.0f, 1.0f, 4.0f, 1.0f};
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(cam_mul, pre_mul, 0, 3, n, reason,
                                      sizeof(reason));
        const bool ok = approx(n[0], 0.5f) && approx(n[1], 1) &&
                        approx(n[2], 0.25f) && approx(n[3], 1) && reason[0] != '\0';
        std::snprintf(detail, sizeof(detail),
                      "cam_mul[0]<-0.5 -> pre_mul neutral=(%.3f,%.3f,%.3f,%.3f) "
                      "want=(0.5,1,0.25,1) reason=\"%s\"",
                      n[0], n[1], n[2], n[3], reason);
        report("wb-legacy-marker-uses-pre_mul", "synthetic", ok, detail);
    }

    // AC-2.1 c: cam_mul all-zero -> unparsed -> pre_mul, with a reason.
    {
        const float cam_mul[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float pre_mul[4] = {2.0f, 1.0f, 4.0f, 1.0f};
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(cam_mul, pre_mul, 0, 3, n, reason,
                                      sizeof(reason));
        const bool ok = approx(n[0], 0.5f) && approx(n[1], 1) &&
                        approx(n[2], 0.25f) && reason[0] != '\0';
        std::snprintf(detail, sizeof(detail),
                      "cam_mul all-zero -> pre_mul neutral=(%.3f,%.3f,%.3f,%.3f) "
                      "reason=\"%s\"", n[0], n[1], n[2], n[3], reason);
        report("wb-zero-cam_mul-uses-pre_mul", "synthetic", ok, detail);
    }

    // AC-2.1 c (channel alignment): cam_mul[0]>0 and [1]>0 but [2] unparsed
    // (<=1e-5). LibRaw's own sentinel tests channels [0]/[2]; the pre-Stage-2
    // predicate tested [0]/[1] and would WRONGLY accept this cam_mul. RED against
    // the stub -> (0.5,1,1,1); new chain falls to pre_mul -> (0.25,1,0.125,1).
    {
        const float cam_mul[4] = {2.0f, 1.0f, 0.0f, 1.0f};
        const float pre_mul[4] = {4.0f, 1.0f, 8.0f, 1.0f};
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(cam_mul, pre_mul, 0, 3, n, reason,
                                      sizeof(reason));
        const bool ok = approx(n[0], 0.25f) && approx(n[1], 1) &&
                        approx(n[2], 0.125f) && reason[0] != '\0';
        std::snprintf(detail, sizeof(detail),
                      "cam_mul[2]<=1e-5 -> pre_mul neutral=(%.3f,%.3f,%.3f,%.3f) "
                      "want=(0.25,1,0.125,1) reason=\"%s\"",
                      n[0], n[1], n[2], n[3], reason);
        report("wb-cam_mul-channel2-alignment", "synthetic", ok, detail);
    }

    // AC-2.1 d + AC-2.4: both cam_mul and pre_mul absent -> identity neutral AND
    // a non-empty reason.
    {
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(nullptr, nullptr, 0, 3, n, reason,
                                      sizeof(reason));
        const bool ok = approx(n[0], 1) && approx(n[1], 1) && approx(n[2], 1) &&
                        approx(n[3], 1) && reason[0] != '\0';
        std::snprintf(detail, sizeof(detail),
                      "both absent -> neutral=(%.3f,%.3f,%.3f,%.3f) reason=\"%s\"",
                      n[0], n[1], n[2], n[3], reason);
        report("wb-both-absent-identity-with-reason", "synthetic", ok, detail);
    }

    // AC-2.2: cam_mul[3] == 0 with colors == 3 -> the 4th channel takes the green
    // gain (m[3] := m[1]), so out_neutral[3] == m[1]/m[1] == 1, never a 0 gain.
    {
        const float cam_mul[4] = {2.0f, 1.0f, 4.0f, 0.0f};
        float n[4] = {-1, -1, -1, -1};
        char reason[256] = {0};
        raw_white_balance_from_libraw(cam_mul, nullptr, 0, /*colors=*/3, n, reason,
                                      sizeof(reason));
        const bool ok = approx(n[0], 0.5f) && approx(n[1], 1) &&
                        approx(n[2], 0.25f) && approx(n[3], 1);
        std::snprintf(detail, sizeof(detail),
                      "cam_mul[3]==0,colors==3 -> neutral=(%.3f,%.3f,%.3f,%.3f) "
                      "want=(0.5,1,0.25,1) [n[3] green gain, not 0]",
                      n[0], n[1], n[2], n[3]);
        report("wb-cam_mul3-zero-green-fill", "synthetic", ok, detail);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

    // Synthetic cases first: they need no corpus file and must run even when
    // every sample is missing (checked==0 still fails the run below).
    checkFlipTable();
    checkBlackFolding();
    checkBayerPlaneOrigin();
    checkFiltersPeriodicity();
    checkMatrixInverse();
    checkRouteTable();
    checkWhiteBalanceChain();

    const std::vector<Sample> samples = loadManifest(manifest);
    dumpRouteCensus(samples);
    std::vector<std::string> bayer_paths;
    std::vector<std::string> bayer_ids;
    std::vector<std::string> xtrans_paths;
    std::vector<std::string> xtrans_ids;
    std::vector<std::string> foveon_paths;
    std::vector<std::string> foveon_ids;

    for (const Sample& s : samples) {
        if (s.id.rfind("malformed_", 0) == 0) continue;
        if (!fileExists(s.path)) {
            std::printf("[LibRawAdapter] SKIP %s (missing file)\n", s.id.c_str());
            continue;
        }

        LibRawFrontendContext ctx;
        if (ctx.open_and_unpack(s.path.c_str()) != kRawSuccess) {
            report("unpack", s.id.c_str(), false, "open_and_unpack failed");
            continue;
        }

        LibRawGpuInputAdapter adapter;
        RawGpuInput input{};
        RawDevelopParams develop{};
        char reason[256] = {0};
        const RawErrorCode rc =
            adapter.build(ctx, &input, &develop, reason, sizeof(reason));

        raw_contract_print("RawInput", &input, rc, reason, stdout);
        ++checked;

        // AC-1.2 on every real file. The route label is recomputed from the view
        // with the SAME predicate the adapter uses, so the printed route is a
        // claim this test can be wrong about -- checkRouteTable() is what pins
        // the predicate itself.
        {
            const LibRawRawView& v = ctx.raw_view();
            bool rgb_cam_identity = true;
            if (v.rgb_cam) {
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        const float want = (r == c) ? 1.0f : 0.0f;
                        if (std::fabs(v.rgb_cam[r * 4 + c] - want) > 1e-6f) {
                            rgb_cam_identity = false;
                        }
                    }
                }
            }
            const bool primary = v.rgb_cam && v.raw_color == 0 &&
                                 v.colors == 3 && !rgb_cam_identity;
            checkPcsWhiteInvariant(s.id.c_str(), input.camera_to_pcs,
                                   primary ? "rgb_cam" : "cam_xyz");
            std::printf("[LibRawAdapter] %s route-observed raw_color=%u colors=%u "
                        "rgb_cam_identity=%d -> %s\n",
                        s.id.c_str(), v.raw_color, v.colors,
                        rgb_cam_identity ? 1 : 0,
                        primary ? "rgb_cam(primary)" : "cam_xyz(fallback)");
        }

        const RawLayoutClass cls = raw_classify_layout(&input.layout);
        char detail[320];

        if (s.expect_layout == "bayer2x2") {
            int32_t rx = -1, ry = -1;
            const int ok_phase = raw_bayer_phase_from_pattern(&input.layout, &rx, &ry);
            std::snprintf(detail, sizeof(detail), "class=%s phase=(%d,%d) rc=%s",
                          raw_layout_class_name(cls), rx, ry, raw_error_name(rc));
            report("", s.id.c_str(),
                   rc == kRawSuccess && cls == kRawLayoutClassBayer2x2 && ok_phase == 1,
                   detail);
            bayer_paths.push_back(s.path);
            bayer_ids.push_back(s.id);
        } else if (s.expect_layout == "xtrans6x6") {
            int greens = 0, reds = 0, blues = 0;
            for (size_t i = 0; i < input.layout.cfa_pattern_count; ++i) {
                const RawColorKey k = input.layout.cfa_pattern[i];
                if (k == kRawColorKeyGreen || k == kRawColorKeyFujiGreen) ++greens;
                else if (k == kRawColorKeyRed) ++reds;
                else if (k == kRawColorKeyBlue) ++blues;
            }
            std::snprintf(detail, sizeof(detail),
                          "class=%s cfa=%ux%u greens=%d reds=%d blues=%d rc=%s",
                          raw_layout_class_name(cls), input.layout.cfa_repeat_width,
                          input.layout.cfa_repeat_height, greens, reds, blues,
                          raw_error_name(rc));
            // rc is kRawErrLayoutUnsupported until Task 12 wires the kernel, so
            // the assertion here is on the descriptor, not on rc.
            report("", s.id.c_str(),
                   cls == kRawLayoutClassXTrans6x6 &&
                       input.layout.cfa_repeat_width == 6 &&
                       input.layout.cfa_repeat_height == 6 &&
                       input.layout.cfa_pattern_count == 36 &&
                       greens == 20 && reds == 8 && blues == 8,
                   detail);
            xtrans_paths.push_back(s.path);
            xtrans_ids.push_back(s.id);
        } else if (s.expect_layout == "linear_rgb") {
            // P19 T8 erratum: this manifest loop predates the linear-RGB
            // route (Phase 17 assumed every non-CFA layout must fail), so the
            // generic else below asserts rc != kRawSuccess -- wrong for the
            // new production layout. Same gap class already fixed in
            // test_raw_end_to_end.cpp's manifest loop; the plan's Task 8 file
            // list omitted this file.
            std::snprintf(detail, sizeof(detail),
                          "class=%s comps=%u planes=%u rc=%s",
                          raw_layout_class_name(cls),
                          input.layout.components_per_pixel,
                          input.layout.plane_count, raw_error_name(rc));
            report("", s.id.c_str(),
                   rc == kRawSuccess && cls == kRawLayoutClassLinearRgb &&
                       input.layout.components_per_pixel == 3 &&
                       input.layout.plane_count == 1,
                   detail);
            foveon_paths.push_back(s.path);
            foveon_ids.push_back(s.id);
        } else {
            std::snprintf(detail, sizeof(detail), "class=%s rc=%s want=%s",
                          raw_layout_class_name(cls), raw_error_name(rc),
                          s.expect_error.c_str());
            report("", s.id.c_str(), rc != kRawSuccess, detail);
        }
    }

    // Backend invariance on one real file.
    //
    // Honesty rule (round-3 finding t6_finding_rawspeed3_eligibility.md):
    // forcing kRawSpeed3 does NOT mean RawSpeed3 ran. image_samples/raw_sample.arw
    // is uncompressed and LibRaw never offers it to RawSpeed3, so a comparison
    // on that file would compare libraw_native against itself and pass
    // vacuously. Pick the first Bayer file whose two forced contexts actually
    // report DIFFERENT unpack backends, and print the observed backend names.
    bool invariance_done = false;
    for (size_t i = 0; i < bayer_paths.size() && !invariance_done; ++i) {
        LibRawFrontendContext rs_ctx, nat_ctx;
        rs_ctx.set_forced_backend(RawForcedBackend::kRawSpeed3);
        nat_ctx.set_forced_backend(RawForcedBackend::kLibRawNative);
        if (rs_ctx.open_and_unpack(bayer_paths[i].c_str()) != kRawSuccess ||
            nat_ctx.open_and_unpack(bayer_paths[i].c_str()) != kRawSuccess) {
            continue;
        }
        const RawDecoderBackend rs_b = rs_ctx.diagnostics().unpack_backend;
        const RawDecoderBackend nat_b = nat_ctx.diagnostics().unpack_backend;
        if (rs_b == nat_b) {
            std::printf("[LibRawAdapter] SKIP backend-invariant %s "
                        "(both forced contexts decoded with %s)\n",
                        bayer_ids[i].c_str(), raw_backend_name(rs_b));
            continue;
        }

        LibRawGpuInputAdapter a1, a2;
        RawGpuInput i1{}, i2{};
        RawDevelopParams d1{}, d2{};
        char r1[256] = {0}, r2[256] = {0};
        const bool built =
            a1.build(rs_ctx, &i1, &d1, r1, sizeof(r1)) == kRawSuccess &&
            a2.build(nat_ctx, &i2, &d2, r2, sizeof(r2)) == kRawSuccess;

        const MetaFingerprint f1 = fingerprint(i1);
        const MetaFingerprint f2 = fingerprint(i2);
        const bool same = built && std::memcmp(&f1, &f2, sizeof(MetaFingerprint)) == 0;
        char detail[256];
        std::snprintf(detail, sizeof(detail), "backends=%s vs %s metadata %s",
                      raw_backend_name(rs_b), raw_backend_name(nat_b),
                      same ? "identical" : "differs");
        report("backend-invariant", bayer_ids[i].c_str(), same, detail);
        invariance_done = true;
    }
    if (!invariance_done) {
        std::printf("[LibRawAdapter] SKIP backend-invariant "
                    "(no Bayer sample decoded by two different backends)\n");
    }

    {
        // P19 (round-5 review F-R5-01): the original version of this case
        // computed component_black[c] = black_scalar + channel_black[c]
        // ITSELF and then asserted its own arithmetic -- deleting the
        // adapter's population code left it green, zero executable coverage.
        // Fixed by extracting the exact expression the adapter uses into a
        // shared, header-declared seam (raw_component_black_from_libraw,
        // libraw_gpu_input_adapter.cpp) and calling THAT here. No .x3f sample
        // exists to drive this through LibRawGpuInputAdapter::build() end to
        // end (SKIP-by-name discipline, corpus has no Foveon file), so the
        // shared-function call is the strongest coverage reachable this round;
        // Task 8's E2E harness is expected to add the real-file path.
        const uint32_t black_scalar = 256u;
        const uint32_t channel_black[4] = {8u, 4u, 12u, 0u};

        float component_black[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        raw_component_black_from_libraw(black_scalar, channel_black, component_black);

        // The spatial tile alongside it must be seeded with a ZERO scalar and
        // no channel term (channel_index == nullptr, cfa_w == cfa_h == 0) --
        // otherwise color.black is subtracted twice and the shadows are
        // crushed. Kept as its own assertion: it is the OTHER half of the
        // arrangement raw_component_black_from_libraw's contract depends on.
        RawBlackLevelPattern tile{};
        char reason[256] = {0};
        const RawErrorCode rc = raw_black_pattern_from_libraw(
            /*black_scalar=*/0u, /*channel_black=*/nullptr,
            /*channel_index=*/nullptr, /*cfa_w=*/0u, /*cfa_h=*/0u,
            /*spatial_black=*/nullptr, /*spatial_w=*/0u, /*spatial_h=*/0u,
            /*left_margin=*/0u, /*top_margin=*/0u, &tile, reason, sizeof(reason));

        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "rc=%s tile=%ux%u tile0=%.1f cb=[%.1f,%.1f,%.1f,%.1f] reason=\"%s\"",
                      raw_error_name(rc), tile.repeat_width, tile.repeat_height,
                      tile.values[0], component_black[0], component_black[1],
                      component_black[2], component_black[3], reason);
        report("linear-rgb-component-black", nullptr,
               rc == kRawSuccess &&
                   tile.repeat_width == 1 && tile.repeat_height == 1 &&
                   tile.values[0] == 0.0f &&
                   component_black[0] == 264.0f &&
                   component_black[1] == 260.0f &&
                   component_black[2] == 268.0f &&
                   component_black[3] == 256.0f,
               detail);

        // channel_black == nullptr must be treated as all-zero, not crash or
        // read garbage -- this is the branch the real adapter takes for any
        // decoder that leaves color.cblack[0..3] untouched.
        float null_channel_black[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        raw_component_black_from_libraw(black_scalar, nullptr, null_channel_black);
        char detail2[128];
        std::snprintf(detail2, sizeof(detail2), "cb=[%.1f,%.1f,%.1f,%.1f]",
                      null_channel_black[0], null_channel_black[1],
                      null_channel_black[2], null_channel_black[3]);
        report("linear-rgb-component-black-null-channel", nullptr,
               null_channel_black[0] == 256.0f && null_channel_black[1] == 256.0f &&
                   null_channel_black[2] == 256.0f && null_channel_black[3] == 256.0f,
               detail2);
    }

    {
        // F-R5-01 (part 2): also exercise the surrounding adapter code that
        // WRITES component_black into a real, adapter-built descriptor. Every
        // corpus sample today is Bayer or X-Trans (sample_model == CFA), so
        // this pins the OTHER half of libraw_gpu_input_adapter.cpp's new
        // logic: the unconditional zero-init and the linear_rgb gate that
        // must NOT fire for a CFA layout. It cannot reach the linear_rgb
        // branch itself without a real .x3f file (none exists in this
        // checkout; SKIP-by-name), which is why the direct seam-function
        // calls above carry the arithmetic correctness gate.
        bool any_cfa_checked = false;
        for (const Sample& s : samples) {
            if (s.id.rfind("malformed_", 0) == 0) continue;
            if (!fileExists(s.path)) continue;
            if (s.expect_layout != "bayer2x2" && s.expect_layout != "xtrans6x6") continue;

            LibRawFrontendContext ctx;
            if (ctx.open_and_unpack(s.path.c_str()) != kRawSuccess) continue;

            LibRawGpuInputAdapter adapter;
            RawGpuInput input{};
            RawDevelopParams develop{};
            char reason[256] = {0};
            if (adapter.build(ctx, &input, &develop, reason, sizeof(reason)) !=
                kRawSuccess) {
                continue;
            }

            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "sample_model=%d cb=[%.1f,%.1f,%.1f,%.1f]",
                          static_cast<int>(input.layout.sample_model),
                          input.component_black[0], input.component_black[1],
                          input.component_black[2], input.component_black[3]);
            report("component-black-zero-for-cfa", s.id.c_str(),
                   input.layout.sample_model == kRawSampleModelCfa &&
                       input.component_black[0] == 0.0f &&
                       input.component_black[1] == 0.0f &&
                       input.component_black[2] == 0.0f &&
                       input.component_black[3] == 0.0f,
                   detail);
            any_cfa_checked = true;
        }
        if (!any_cfa_checked) {
            std::printf("[LibRawAdapter] SKIP component-black-zero-for-cfa "
                        "(no CFA sample built successfully)\n");
        }
    }

    // Round 1 Task 1.3 acceptance, promoted per-format by Task 1.6: on one
    // corpus file per format (Bayer, X-Trans, Foveon), the adapter returns
    // auto_exposure_ev finite and in [0,2] with the mode on (default), and
    // exactly 0.0f with the mode off. X-Trans additionally asserts auto_ev
    // > 0 on the real corpus file -- the whole point of generalising past
    // Bayer; if it computed exactly 0.0 that would need investigating before
    // being accepted as a passing case, not asserted around.
    struct FormatCase { const char* label; const std::vector<std::string>* paths;
                        const std::vector<std::string>* ids; };
    const FormatCase format_cases[] = {
        {"bayer", &bayer_paths, &bayer_ids},
        {"xtrans", &xtrans_paths, &xtrans_ids},
        {"foveon", &foveon_paths, &foveon_ids},
    };
    for (const FormatCase& fc : format_cases) {
        if (fc.paths->empty()) {
            std::printf("[LibRawAdapter] SKIP auto-ev-populated-%s (no %s sample built)\n",
                        fc.label, fc.label);
            continue;
        }
        const std::string& path = fc.paths->front();
        const std::string& id = fc.ids->front();

        LibRawFrontendContext ctx_on;
        LibRawGpuInputAdapter adapter_on;
        RawGpuInput input_on{};
        RawDevelopParams dev_on{};
        char reason_on[256] = {0};
        const bool ok_on =
            ctx_on.open_and_unpack(path.c_str()) == kRawSuccess &&
            adapter_on.build(ctx_on, &input_on, &dev_on, reason_on, sizeof(reason_on)) ==
                kRawSuccess;
        char detail_on[200];
        std::snprintf(detail_on, sizeof(detail_on),
                      "ok=%d auto_exposure_mode=%d auto_exposure_ev=%.7f (%s)",
                      ok_on, dev_on.auto_exposure_mode, dev_on.auto_exposure_ev, reason_on);
        std::string name_on = std::string("auto-ev-populated-on-") + fc.label;
        report(name_on.c_str(), id.c_str(),
               ok_on && dev_on.auto_exposure_mode == kRawAutoExposureOn &&
                   std::isfinite(dev_on.auto_exposure_ev) &&
                   dev_on.auto_exposure_ev >= 0.0f && dev_on.auto_exposure_ev <= 2.0f,
               detail_on);

        // Prior to Task 1.7 (b5e90a2) this block additionally asserted
        // auto_exposure_ev > 0.0f on fuji_xt3, pinned to the withdrawn
        // raw-domain formula. The output-domain bisection solve now
        // correctly computes 0.0 on that file (it renders 10.6% brighter
        // than the LibRaw oracle even without gain -- no headroom to add),
        // so that magnitude assertion is stale and has been removed.
        // RawDevelopParams carries no solver-status field to assert kOk
        // directly (auto_exposure_ev is output-only by design, see the
        // header), so what "the X-Trans path executed and classified rather
        // than being silently skipped" reduces to observably is exactly the
        // auto-ev-populated-on/-off pair below: ok_on true, a finite value
        // in [0,2] with the mode on, and exactly 0.0f with the mode off --
        // kUnsupportedLayout is in any case structurally unreachable for a
        // real corpus file here, since this adapter's colour_of_site
        // construction folds every RawColorKey into 0..2 and never emits a
        // table entry >= 3, and kNoRenderEval cannot occur because the
        // adapter always passes the non-null production raw_render_eval
        // callback.

        LibRawFrontendContext ctx_off;
        LibRawGpuInputAdapter adapter_off;
        RawGpuInput input_off{};
        RawDevelopParams dev_off{};
        dev_off.auto_exposure_mode = kRawAutoExposureOff;
        char reason_off[256] = {0};
        const bool ok_off =
            ctx_off.open_and_unpack(path.c_str()) == kRawSuccess &&
            adapter_off.build(ctx_off, &input_off, &dev_off, reason_off, sizeof(reason_off)) ==
                kRawSuccess;
        char detail_off[200];
        std::snprintf(detail_off, sizeof(detail_off),
                      "ok=%d auto_exposure_mode=%d auto_exposure_ev=%.7f (%s)",
                      ok_off, dev_off.auto_exposure_mode, dev_off.auto_exposure_ev, reason_off);
        std::string name_off = std::string("auto-ev-populated-off-") + fc.label;
        report(name_off.c_str(), id.c_str(),
               ok_off && dev_off.auto_exposure_mode == kRawAutoExposureOff &&
                   dev_off.auto_exposure_ev == 0.0f,
               detail_off);
    }

    if (checked == 0) {
        std::printf("[LibRawAdapter] FAIL no corpus files were present\n");
        return 1;
    }
    if (failures != 0) {
        std::printf("[LibRawAdapter] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[LibRawAdapter] ALL PASS\n");
    return 0;
}
