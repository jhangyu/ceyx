// LibRawGpuInputAdapter coverage.
//
// Two properties carry the architecture:
//   1. X-Trans keeps the full 6x6 pattern (a filters==9 collapse loses it).
//   2. The contract is backend-invariant: RawSpeed3 and forced-native unpacks
//      of the SAME file must yield identical layout/rect/orientation/black/
//      white/matrix bytes. Only pixels may differ.
//
// Usage: --manifest <path>. Missing corpus files are SKIPped by name.
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

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

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
