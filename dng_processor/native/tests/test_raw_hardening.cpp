// Malformed input, resource limits and cancellation (spec section 10).
//
// The property that matters on every failure path is the same: a specific
// error code, no crash, no hang, and NO partially populated RGBA buffer
// (spec section 10.3). A decode that returns half an image is worse than one
// that fails, because callers cannot tell.
//
// Corpus resolution (deviation from the plan's reference code, which hard-codes
// image_samples/raw_corpus/sony_a7r4.arw): that file does not exist in this
// checkout, so the Bayer sample is resolved FROM THE MANIFEST instead - the
// first present entry whose expect_route is generic and expect_layout is
// bayer2x2. Hard-coding the plan's path would silently downgrade the
// cancellation and GPU-mandatory criteria to SKIPs.
//
// The three malformed fixtures live at
// image_samples/raw_corpus/<basename of that sample>.{trunc,pitch,cfa}.raw and
// are produced by scripts/tmp/r7_t13_gen_fixtures.py, which applies exactly the
// transforms of tests/verify_raw_corpus.py --generate-malformed but anchors
// them to the GENERIC Bayer sample: the stock helper picks the first present
// bayer2x2 entry, which here is the DNG one (frontend_only route), and fixtures
// derived from that would exercise the Adobe DNG SDK path instead of the LibRaw
// path this test is hardening.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "dng_pipeline_v2.h"
#include "raw_gpu_pipeline.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    std::printf("[RawHardening] %s %s -> %s\n", name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

bool fileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

// Same minimal object scanner the other RAW tests use: the manifest is a flat
// list of one-level JSON objects, so a brace scan plus a key lookup is enough
// and keeps the test dependency-free.
std::string field(const std::string& obj, const char* key) {
    const std::string needle = std::string("\"") + key + "\": \"";
    const size_t at = obj.find(needle);
    if (at == std::string::npos) return "";
    const size_t start = at + needle.size();
    const size_t end = obj.find('"', start);
    return end == std::string::npos ? "" : obj.substr(start, end - start);
}

std::string resolveBayerSample(const char* manifest_path) {
    std::ifstream in(manifest_path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    size_t pos = 0;
    while ((pos = text.find('{', pos)) != std::string::npos) {
        const size_t end = text.find('}', pos);
        if (end == std::string::npos) break;
        const std::string obj = text.substr(pos, end - pos);
        pos = end + 1;
        if (field(obj, "expect_route") != "generic") continue;
        if (field(obj, "expect_layout") != "bayer2x2") continue;
        const std::string path = field(obj, "path");
        if (!path.empty() && fileExists(path)) return path;
    }
    return "";
}

// Every malformed case asserts the same three things, so a new fixture is one
// line rather than a copied block.
void expectCleanFailure(const char* case_name, const char* path) {
    RawDevelopParams develop{};
    develop.tone_curve_strength = 1.0f;
    RawPipelineResult out;
    const RawErrorCode rc = raw_pipeline_decode_file(path, develop, out);

    const size_t checked_out = dng_rgba_output_checked_out_count();
    char detail[280];
    std::snprintf(detail, sizeof(detail), "error=%s rgba=%s pool=%zu",
                  raw_error_name(rc), out.rgba_ptr ? "NON-NULL" : "null", checked_out);
    report(case_name, rc != kRawSuccess && out.rgba_ptr == nullptr && checked_out == 0,
           detail);
    if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);
}

int alwaysCancel(void*) { return 1; }
int neverCancel(void*) { return 0; }

struct DispatchCounter { int calls = 0; };

// The route polls at four points: after the probe (before the decoder is
// opened), between open_file and unpack, after unpack, and immediately before
// the GPU dispatch. This callback lets the first three through and cancels at
// the fourth, i.e. the LATEST point at which cancellation is honoured: the
// decoder is open, the pixels are unpacked and borrowed, and the very next
// statement would be the dispatch. That is the case that exercises the recycle
// ordering of spec section 5.2.5.
//
// Cancellation is deliberately NOT honoured once raw_pipeline_decode_to_rgba
// has been entered (plan Task 13, step 4): the shared Stage4 call blocks until
// the GPU command completes, and returning earlier would free borrowed pixels
// the GPU is still reading. So "after dispatch" cannot be requested from
// outside; what a caller can do is cancel at the last pre-dispatch poll, and
// the invariant to prove is that this still leaves nothing half-populated.
constexpr int kPreDispatchPollOrdinal = 4;

int cancelAtLastPoll(void* user) {
    DispatchCounter* counter = static_cast<DispatchCounter*>(user);
    return (++counter->calls >= kPreDispatchPollOrdinal) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0) manifest = argv[i + 1];
    }

    const auto t_start = std::chrono::steady_clock::now();

    const std::string bayer = resolveBayerSample(manifest);
    std::printf("[RawHardening] corpus manifest=%s bayer=%s\n", manifest,
                bayer.empty() ? "(none present)" : bayer.c_str());

    // 1. Degenerate paths.
    expectCleanFailure("null_path", nullptr);
    expectCleanFailure("nonexistent_path", "image_samples/raw_corpus/does_not_exist.arw");
    expectCleanFailure("directory_path", "image_samples/raw_corpus");

    // 2. Synthetic garbage files, written next to the corpus (untracked).
    {
        const std::string zero = "image_samples/raw_corpus/zero_byte.raw";
        std::ofstream(zero, std::ios::binary).close();
        expectCleanFailure("zero_byte_file", zero.c_str());

        const std::string rnd = "image_samples/raw_corpus/random_512.raw";
        {
            std::ofstream f(rnd, std::ios::binary);
            uint32_t state = 0xDEADBEEFu;
            for (int i = 0; i < 512; ++i) {
                state = state * 1664525u + 1013904223u;
                const char byte = static_cast<char>((state >> 16) & 0xFF);
                f.write(&byte, 1);
            }
        }
        expectCleanFailure("random_512_bytes", rnd.c_str());
    }

    // 3. Generated malformed fixtures, derived from the resolved Bayer sample.
    {
        const size_t slash = bayer.find_last_of('/');
        const std::string base = bayer.empty() ? std::string()
            : std::string("image_samples/raw_corpus/") +
              (slash == std::string::npos ? bayer : bayer.substr(slash + 1));
        const char* suffixes[][2] = {
            {"truncated_60pct", ".trunc.raw"},
            {"patched_pitch",   ".pitch.raw"},
            {"zeroed_cfa",      ".cfa.raw"},
        };
        for (const auto& kase : suffixes) {
            const std::string path = base.empty() ? std::string()
                                                  : base + kase[1];
            if (path.empty() || !fileExists(path)) {
                std::printf("[RawHardening] SKIP %s (missing fixture; run "
                            "python3 dng_processor/native/scripts/tmp/"
                            "r7_t13_gen_fixtures.py)\n", kase[0]);
                continue;
            }
            expectCleanFailure(kase[0], path.c_str());
        }
    }

    // 4. Oversized declared dimensions must be refused before allocation.
    {
        static uint16_t storage[64];
        RawPlaneView plane{};
        plane.data = storage;
        plane.byte_size = sizeof(storage);
        plane.width = 65535;
        plane.height = 65535;      // 4.29e9 pixels > kRawMaxPixelCount
        plane.row_stride_bytes = 131070;
        plane.pixel_stride_bytes = 2;

        static RawColorKey pattern[4] = {kRawColorKeyRed, kRawColorKeyGreen,
                                         kRawColorKeyGreen, kRawColorKeyBlue};
        RawGpuInput in{};
        in.planes = &plane; in.plane_count = 1;
        in.layout.sample_model = kRawSampleModelCfa;
        in.layout.sample_type = kRawSampleTypeU16;
        in.layout.plane_count = 1;
        in.layout.components_per_pixel = 1;
        in.layout.cfa_repeat_width = 2;
        in.layout.cfa_repeat_height = 2;
        in.layout.cfa_pattern = pattern;
        in.layout.cfa_pattern_count = 4;
        in.active_area = RawRect{0, 0, 65535, 65535};
        in.default_crop = RawRect{0, 0, 65535, 65535};
        in.orientation = kRawOrientationTopLeft;
        in.black.repeat_width = 1; in.black.repeat_height = 1;
        in.black.values[0] = 0.0f;
        for (int i = 0; i < 4; ++i) {
            in.white_level[i] = 16383.0f;
            in.as_shot_neutral[i] = 1.0f;
        }
        in.camera_to_pcs.valid = 1;
        in.camera_to_pcs.out_rows = 3; in.camera_to_pcs.in_cols = 3;
        for (int i = 0; i < 9; ++i) in.camera_to_pcs.m[i] = (i % 4 == 0) ? 1.0f : 0.0f;

        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        RawPipelineResult out;
        const RawErrorCode rc = raw_pipeline_decode_to_rgba(in, develop, out);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "error=%s rgba=%s",
                      raw_error_name(rc), out.rgba_ptr ? "NON-NULL" : "null");
        report("oversize-dimensions",
               rc == kRawErrSizeOverflow && out.rgba_ptr == nullptr, detail);
        if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);
    }

    // 5. Cancellation, before and after dispatch.
    if (bayer.empty()) {
        std::printf("[RawHardening] SKIP cancellation (no Bayer sample)\n");
    } else {
        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;

        RawCancelToken early;
        early.callback = alwaysCancel;
        early.user_data = nullptr;
        RawPipelineResult out_early;
        const RawErrorCode rc_early = raw_pipeline_decode_file_cancellable(
            bayer.c_str(), develop, early, out_early);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "error=%s rgba=%s pool=%zu",
                      raw_error_name(rc_early),
                      out_early.rgba_ptr ? "NON-NULL" : "null",
                      dng_rgba_output_checked_out_count());
        report("cancel-before-dispatch",
               rc_early == kRawErrKernelFailed && out_early.rgba_ptr == nullptr &&
                   dng_rgba_output_checked_out_count() == 0,
               detail);
        if (out_early.rgba_ptr) dng_rgba_output_release(out_early.rgba_ptr);

        DispatchCounter counter;
        RawCancelToken late;
        late.callback = cancelAtLastPoll;
        late.user_data = &counter;
        RawPipelineResult out_late;
        const RawErrorCode rc_late = raw_pipeline_decode_file_cancellable(
            bayer.c_str(), develop, late, out_late);
        // Whatever the verdict, the GPU command must have completed before the
        // LibRaw processor was recycled: reading the output buffer here must not
        // fault (see spec section 5.2.5). Under ASan this is the assertion that
        // would catch a use-after-free of the pool buffer.
        volatile uint8_t probe = 0;
        if (out_late.rgba_ptr) probe = out_late.rgba_ptr[0];
        (void)probe;
        const size_t late_pool = dng_rgba_output_checked_out_count();
        std::snprintf(detail, sizeof(detail),
                      "error=%s polls=%d rgba=%s pool=%zu",
                      raw_error_name(rc_late), counter.calls,
                      out_late.rgba_ptr ? "NON-NULL" : "null", late_pool);
        // The token must have survived to the pre-dispatch poll (otherwise the
        // later poll points are dead code), the verdict must be the specific
        // cancellation error, and nothing may be left half-populated.
        report("cancel-after-dispatch",
               counter.calls >= kPreDispatchPollOrdinal &&
                   rc_late == kRawErrKernelFailed &&
                   out_late.rgba_ptr == nullptr && late_pool == 0,
               detail);
        if (out_late.rgba_ptr) dng_rgba_output_release(out_late.rgba_ptr);

        // The uncancelled control must still succeed.
        RawCancelToken none;
        none.callback = neverCancel;
        none.user_data = nullptr;
        RawPipelineResult out_ok;
        const RawErrorCode rc_ok =
            raw_pipeline_decode_file_cancellable(bayer.c_str(), develop, none, out_ok);
        report("cancel-token-noop", rc_ok == kRawSuccess && out_ok.rgba_ptr != nullptr,
               "uncancelled decode still succeeds");
        if (out_ok.rgba_ptr) dng_rgba_output_release(out_ok.rgba_ptr);
    }

    // 6. GPU-mandatory: no CPU render fallback (spec section 2.6).
    if (bayer.empty()) {
        std::printf("[RawHardening] SKIP gpu-unavailable (no Bayer sample)\n");
    } else {
        setenv("DNG_RAW_FORCE_GPU_UNAVAILABLE", "1", 1);
        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        RawPipelineResult out;
        const RawErrorCode rc = raw_pipeline_decode_file(bayer.c_str(), develop, out);
        const size_t pool = dng_rgba_output_checked_out_count();
        char detail[200];
        std::snprintf(detail, sizeof(detail), "error=%s rgba=%s pool=%zu",
                      raw_error_name(rc), out.rgba_ptr ? "NON-NULL" : "null", pool);
        report("gpu-unavailable",
               rc == kRawErrGpuUnavailable && out.rgba_ptr == nullptr && pool == 0,
               detail);
        if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);

        // Control: with the override cleared the same file must decode, so the
        // failure above is attributable to the switch and not to a broken build.
        unsetenv("DNG_RAW_FORCE_GPU_UNAVAILABLE");
        RawPipelineResult ok_out;
        const RawErrorCode ok_rc = raw_pipeline_decode_file(bayer.c_str(), develop, ok_out);
        char ok_detail[200];
        std::snprintf(ok_detail, sizeof(ok_detail), "error=%s rgba=%s size=%ux%u",
                      raw_error_name(ok_rc), ok_out.rgba_ptr ? "NON-NULL" : "null",
                      ok_out.width, ok_out.height);
        report("gpu-available-control",
               ok_rc == kRawSuccess && ok_out.rgba_ptr != nullptr, ok_detail);
        if (ok_out.rgba_ptr) dng_rgba_output_release(ok_out.rgba_ptr);
    }

    // Nothing may stay checked out, on success or failure.
    {
        char detail[120];
        const size_t left = dng_rgba_output_checked_out_count();
        std::snprintf(detail, sizeof(detail), "rgba_checked_out=%zu", left);
        report("pool-leak", left == 0, detail);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count();
    char detail[96];
    std::snprintf(detail, sizeof(detail), "elapsed=%llds", static_cast<long long>(elapsed));
    report("no-hang", elapsed < 30, detail);

    if (failures != 0) {
        std::printf("[RawHardening] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawHardening] ALL PASS\n");
    return 0;
}
