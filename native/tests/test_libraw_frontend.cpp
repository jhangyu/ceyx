// LibRawFrontendContext coverage: one processor, open_file + unpack, borrowed
// view, backend observability, and the forced-backend switch that proves both
// decoders really run on the same file.
//
// Usage: --manifest <path>. Missing corpus files are SKIPped by name; a
// present file that misbehaves is a FAIL.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "libraw_frontend.h"

namespace {

int failures = 0;
int checked = 0;

void report(const char* name, const char* id, bool ok, const char* detail) {
    std::printf("[LibRawFrontend] %s%s%s %s -> %s\n", id ? id : "", id ? " " : "",
                name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

struct Sample {
    std::string id;
    std::string path;
    std::string expect_backend;
    std::string expect_error;
    std::string expect_layout;
};

// Deliberately a minimal hand-rolled reader: the test must not gain a JSON
// dependency, and the manifest fields it needs are flat strings.
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
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
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

bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Round-3 review finding F2: the frontend used to accept ANY non-null
// raw_image. LibRaw aliases raw_image onto the 4-component imgdata.image for
// the sRAW / legacy decoders (third_party/libraw/src/decoders/unpack.cpp:436,
// raw_pitch = width*8), producing a borrowed view whose stride implies 8 bytes
// per pixel while the view claims 2 — and raw_validate_gpu_input cannot catch
// it, because an over-large stride passes every stride rule.
//
// No in-repo corpus file uses a legacy decoder, so the gate is covered as a
// truth table over LibRaw's own allocation predicate (unpack.cpp:382). Cases
// marked [F2] are the ones that the pre-fix `raw_image != nullptr` logic gets
// wrong; the rest pin down what must keep being accepted.
void checkAcceptanceGate() {
    // Distinct, non-null addresses standing in for LibRaw's buffers.
    unsigned short store_a = 0, store_b = 0;
    const void* const kAllocA = &store_a;
    const void* const kAllocB = &store_b;
    const uint32_t kBayerFilters = 0x94949494u;   // RGGB
    const uint32_t kXTransFilters = 9u;

    struct Case {
        const char* id;
        const void* raw_alloc;
        const void* raw_image;
        uint32_t filters;
        uint32_t colors;
        bool expect_accept;
    };

    const Case cases[] = {
        // Accepted: LibRaw's own Bayer branch (unpack.cpp:392-398).
        {"gate-bayer-own-store", kAllocA, kAllocA, kBayerFilters, 3, true},
        {"gate-xtrans-own-store", kAllocA, kAllocA, kXTransFilters, 3, true},
        // Accepted: monochrome (colors == 1) is the second half of :382.
        {"gate-monochrome", kAllocA, kAllocA, 0, 1, true},
        // Accepted: RawSpeed3 assigns raw_image = rs3ret.pixeldata and leaves
        // raw_alloc null (unpack.cpp:189). Also the phase-one reuse path
        // (src/utils/phaseone_processing.cpp:35-37). Must NOT be rejected.
        {"gate-rawspeed3-no-alloc", nullptr, kAllocA, kBayerFilters, 3, true},
        // [F2] sRAW / legacy: raw_alloc = 0 and raw_image aliases the
        // 4-component imgdata.image (unpack.cpp:429-437).
        {"gate-sraw-alias-3color", nullptr, kAllocA, 0, 3, false},
        {"gate-legacy-alias-4color", nullptr, kAllocA, 0, 4, false},
        // [F2] a raw store exists but raw_image is not it: color3/color4/float
        // allocations that leave a stale raw_image pointer behind.
        {"gate-alloc-image-mismatch", kAllocB, kAllocA, kBayerFilters, 3, false},
        // Pre-existing behaviour: nothing decoded into raw_image.
        {"gate-null-raw-image", kAllocA, nullptr, kBayerFilters, 3, false},
    };

    for (const Case& c : cases) {
        const bool got = raw_frontend_pixels_live_in_raw_image(
            c.raw_alloc, c.raw_image, c.filters, c.colors);
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "alloc=%s image=%s filters=0x%x colors=%u -> accept=%d want=%d",
                      c.raw_alloc ? "set" : "null", c.raw_image ? "set" : "null",
                      c.filters, c.colors, got ? 1 : 0, c.expect_accept ? 1 : 0);
        report("", c.id, got == c.expect_accept, detail);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

    // Corpus-independent: the F2 acceptance gate truth table.
    checkAcceptanceGate();

    const std::vector<Sample> samples = loadManifest(manifest);
    if (samples.empty()) {
        std::printf("[LibRawFrontend] FAIL manifest %s yielded no samples\n", manifest);
        return 1;
    }

    std::string first_bayer;
    for (const Sample& s : samples) {
        if (!fileExists(s.path)) {
            std::printf("[LibRawFrontend] SKIP %s (missing file)\n", s.id.c_str());
            continue;
        }
        if (s.id.rfind("malformed_", 0) == 0) continue;  // Task 13 owns these

        LibRawFrontendContext ctx;
        const RawErrorCode rc = ctx.open_and_unpack(s.path.c_str());
        char detail[256];

        if (s.expect_error == "kRawSuccess" || s.expect_error == "kRawErrLayoutUnsupported") {
            // Both cases must UNPACK successfully; layout rejection happens later.
            const RawDecodeDiagnostics& d = ctx.diagnostics();
            const char* got = raw_backend_name(d.unpack_backend);
            std::snprintf(detail, sizeof(detail),
                          "backend=%s pitch=%lld want_backend=%s rc=%s",
                          got, static_cast<long long>(ctx.raw_view().plane.row_stride_bytes),
                          s.expect_backend.c_str(), raw_error_name(rc));
            const bool ok = (rc == kRawSuccess) &&
                            (s.expect_backend.empty() || s.expect_backend == got);
            report("", s.id.c_str(), ok, detail);
            ++checked;

            if (rc == kRawSuccess) {
                report("frontend-is-libraw", s.id.c_str(),
                       d.frontend == kRawFrontendLibRaw, "frontend=libraw");
                report("unpack-timed", s.id.c_str(), d.raw_unpack_ms > 0.0,
                       "raw_unpack_ms>0");
            }
            if (s.expect_layout == "bayer2x2" && first_bayer.empty()) {
                first_bayer = s.path;
            }
        }
    }

    // Borrowed view: no repack, and the stride is LibRaw's raw_pitch verbatim.
    if (!first_bayer.empty()) {
        LibRawFrontendContext ctx;
        const RawErrorCode rc = ctx.open_and_unpack(first_bayer.c_str());
        const RawDecodeDiagnostics& d = ctx.diagnostics();
        const RawPlaneView& v = ctx.raw_view().plane;
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "repack_bytes=%lld stride=%lld width=%u",
                      static_cast<long long>(d.raw_repack_bytes),
                      static_cast<long long>(v.row_stride_bytes), v.width);
        report("borrowed-view no-repack", nullptr,
               rc == kRawSuccess && d.raw_repack_bytes == 0 &&
                   v.row_stride_bytes >= static_cast<int64_t>(v.width) * 2,
               detail);

        // Both backends really execute on the same file.
        LibRawFrontendContext rs_ctx;
        rs_ctx.set_forced_backend(RawForcedBackend::kRawSpeed3);
        const RawErrorCode rs_rc = rs_ctx.open_and_unpack(first_bayer.c_str());

        LibRawFrontendContext nat_ctx;
        nat_ctx.set_forced_backend(RawForcedBackend::kLibRawNative);
        const RawErrorCode nat_rc = nat_ctx.open_and_unpack(first_bayer.c_str());

        const RawDecoderBackend rs_b = rs_ctx.diagnostics().unpack_backend;
        const RawDecoderBackend nat_b = nat_ctx.diagnostics().unpack_backend;
        std::snprintf(detail, sizeof(detail), "forced_rawspeed=%s forced_native=%s",
                      raw_backend_name(rs_b), raw_backend_name(nat_b));
        report("forced-backend both-executed", nullptr,
               rs_rc == kRawSuccess && nat_rc == kRawSuccess &&
                   rs_b == kRawDecoderBackendRawSpeed3 &&
                   nat_b == kRawDecoderBackendLibRawNative,
               detail);

        // recycle() must be idempotent and must close the view.
        LibRawFrontendContext lifecycle;
        lifecycle.open_and_unpack(first_bayer.c_str());
        lifecycle.recycle();
        lifecycle.recycle();
        report("recycle-idempotent", nullptr, !lifecycle.is_open(), "is_open=false");
    } else {
        std::printf("[LibRawFrontend] SKIP borrowed-view/forced-backend "
                    "(no Bayer sample present)\n");
    }

    {
        // P19: X3F pixels land in rawdata.color3_image, never rawdata.raw_image
        // (third_party/libraw/src/x3f/x3f_parse_process.cpp:588-640). This
        // truth table is the whole acceptance rule, and it needs no sample file
        // -- which is the point, because this checkout has no .x3f.
        int dummy_alloc = 0;
        int dummy = 0;
        void* alloc = &dummy_alloc;
        void* other = &dummy;   // any distinct address

        const struct {
            const char* name;
            const void* raw_alloc;
            const void* color3;
            uint32_t filters;
            uint32_t colors;
            bool want;
        } cases[] = {
            {"x3f_normal",          alloc,   alloc,   0u, 3u, true},
            {"null_color3",         alloc,   nullptr, 0u, 3u, false},
            {"alloc_elsewhere",     other,   alloc,   0u, 3u, false},
            {"no_alloc_recorded",   nullptr, alloc,   0u, 3u, true},
            {"cfa_contradiction",   alloc,   alloc,   9u, 3u, false},
            {"bayer_contradiction", alloc,   alloc,   0x94949494u, 3u, false},
            {"four_components",     alloc,   alloc,   0u, 4u, false},
            {"one_component",       alloc,   alloc,   0u, 1u, false},
        };

        for (const auto& c : cases) {
            const bool got = raw_frontend_pixels_live_in_color3_image(
                c.raw_alloc, c.color3, c.filters, c.colors);
            char detail[160];
            std::snprintf(detail, sizeof(detail), "case=%s got=%d want=%d",
                          c.name, static_cast<int>(got), static_cast<int>(c.want));
            report("color3-predicate-truth-table", nullptr, got == c.want, detail);
        }
    }

    if (checked == 0) {
        std::printf("[LibRawFrontend] FAIL no corpus files were present\n");
        return 1;
    }
    if (failures != 0) {
        std::printf("[LibRawFrontend] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[LibRawFrontend] ALL PASS\n");
    return 0;
}
