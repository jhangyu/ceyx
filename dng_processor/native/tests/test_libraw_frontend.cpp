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

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

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
