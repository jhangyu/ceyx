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
uint32_t librawEffectiveBlack(uint32_t scalar, const uint32_t cb[4],
                              const uint8_t* cfa, uint32_t cfa_w, uint32_t cfa_h,
                              const uint32_t* sp, uint32_t sp_w, uint32_t sp_h,
                              uint32_t row, uint32_t col) {
    uint32_t v = scalar;
    if (cfa && cfa_w && cfa_h) v += cb[cfa[(row % cfa_h) * cfa_w + (col % cfa_w)]];
    if (sp && sp_w && sp_h) v += sp[(row % sp_h) * sp_w + (col % sp_w)];
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
                       uint32_t want_tile_w, uint32_t want_tile_h) {
    RawBlackLevelPattern black{};
    char reason[256] = {0};
    const RawErrorCode rc = raw_black_pattern_from_libraw(
        scalar, cb, cfa, cfa_w, cfa_h, sp, sp_w, sp_h, &black, reason,
        sizeof(reason));

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
                    scalar, cb, cfa, cfa_w, cfa_h, sp, sp_w, sp_h, r, c));
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
    checkOneBlackCase("channel-only", 100, kChan, kRggb, 2, 2, nullptr, 0, 0, 2, 2);

    // 2. per-channel + 2x2 spatial: both terms, same period.
    static const uint32_t kSp2x2[4] = {1, 2, 3, 4};
    checkOneBlackCase("channel-plus-spatial-2x2", 100, kChan, kRggb, 2, 2, kSp2x2,
                      2, 2, 2, 2);

    // 3. co-prime periods: 2x2 CFA against a 3x3 spatial tile -> lcm 6x6.
    static const uint32_t kSp3x3[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    checkOneBlackCase("channel-2x2-spatial-3x3-lcm", 50, kChan, kRggb, 2, 2, kSp3x3,
                      3, 3, 6, 6);

    // 4. X-Trans 6x6 channel tile against a 2x2 spatial tile -> lcm 6x6, which
    //    is 36 entries and still inside the 64-entry values[] array.
    static uint8_t xtrans[36];
    for (int i = 0; i < 36; ++i) xtrans[i] = static_cast<uint8_t>(i % 3);
    checkOneBlackCase("xtrans-6x6-spatial-2x2", 512, kChan, xtrans, 6, 6, kSp2x2,
                      2, 2, 6, 6);

    // 5. all-zero cblack[0..3] must NOT enlarge the tile: this is what keeps the
    //    present corpus byte-identical to round 4.
    checkOneBlackCase("zero-channel-stays-1x1", 800, kZeroChan, kRggb, 2, 2,
                      nullptr, 0, 0, 1, 1);
    checkOneBlackCase("zero-channel-keeps-spatial", 800, kZeroChan, kRggb, 2, 2,
                      kSp2x2, 2, 2, 2, 2);

    // 6. a combined period that does not fit must FAIL LOUD, never truncate.
    static const uint32_t kSp4x4[16] = {0};
    RawBlackLevelPattern black{};
    char reason[256] = {0};
    const RawErrorCode rc = raw_black_pattern_from_libraw(
        0, kChan, xtrans, 6, 6, kSp4x4, 4, 4, &black, reason, sizeof(reason));
    char detail[320];
    std::snprintf(detail, sizeof(detail), "6x6 CFA vs 4x4 spatial -> rc=%s (%s)",
                  raw_error_name(rc), reason);
    report("oversized-lcm-rejected", "cblack", rc == kRawErrLayoutUnsupported,
           detail);
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

// On a real file: the emitted matrix must be the INVERSE of LibRaw's cam_xyz,
// i.e. cam_xyz * emitted == I. If the adapter ever reverts to transcribing
// cam_xyz directly this fails immediately.
void checkMatrixDirection(const char* id, const LibRawRawView& v,
                          const RawGpuInput& in) {
    if (!in.camera_to_pcs.valid || !v.cam_xyz) {
        std::printf("[LibRawAdapter] SKIP matrix-direction %s (no matrix)\n", id);
        return;
    }
    float cam_from_xyz[9];
    for (int i = 0; i < 9; ++i) cam_from_xyz[i] = v.cam_xyz[i];
    float prod[9] = {0};
    mul3x3(cam_from_xyz, in.camera_to_pcs.m, prod);
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "cam_xyz * emitted diag=(%.6f,%.6f,%.6f) off=(%.6f,%.6f)",
                  prod[0], prod[4], prod[8], prod[1], prod[5]);
    report("matrix-direction", id, nearIdentity3x3(prod, 1e-3f), detail);
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

    // Synthetic cases first: they need no corpus file and must run even when
    // every sample is missing (checked==0 still fails the run below).
    checkFlipTable();
    checkBlackFolding();
    checkMatrixInverse();

    const std::vector<Sample> samples = loadManifest(manifest);
    std::vector<std::string> bayer_paths;
    std::vector<std::string> bayer_ids;

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

        checkMatrixDirection(s.id.c_str(), ctx.raw_view(), input);

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
