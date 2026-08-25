// Generic RAW route, end to end: router -> LibRaw frontend -> adapter ->
// fused normalize+demosaic -> shared Stage4 -> RGBA pool -> FFI.
//
// Task 12 ships X-Trans as a product route, so the former
// "xtrans-unsupported" expectation is now a success expectation, and every
// remaining layout class is asserted to route to an explicit
// kRawErrLayoutUnsupported instead of reaching any kernel.
//
// No RAF sample exists in this checkout (both manifest entries are absent), so
// the file-driven X-Trans cases SKIP. The synthetic X-Trans case below is the
// runtime coverage of the X-Trans branch: it drives raw_pipeline_decode_to_rgba
// with an in-memory canonical-tile mosaic and needs no corpus file.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "dng_ffi_api.h"
#include "dng_pipeline_v2.h"
#include "libraw_gpu_input_adapter.h"
#include "raw_contract_validate.h"
#include "raw_gpu_pipeline.h"

namespace {

int failures = 0;
int checked = 0;

void report(const char* name, const char* id, bool ok, const char* detail) {
    std::printf("[RawE2E] %s%s%s %s -> %s\n", id ? id : "", id ? " " : "",
                name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

struct Sample {
    std::string id, path, route, expect_backend, expect_error, expect_layout;
};

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
        Sample s{field(obj, "id"),             field(obj, "path"),
                 field(obj, "expect_route"),   field(obj, "expect_backend"),
                 field(obj, "expect_error"),   field(obj, "expect_layout")};
        if (!s.id.empty() && !s.path.empty()) out.push_back(s);
        pos = end + 1;
    }
    return out;
}

bool fileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

bool alphaAll255(const uint8_t* rgba, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        if (rgba[i * 4 + 3] != 255) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal SHA-256 (FIPS 180-4). Self-contained so the corpus hash gate needs no
// extra dependency and behaves identically on every platform.
// ---------------------------------------------------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t block[64] = {};
    size_t block_len = 0;
    uint64_t total_bits = 0;

    static uint32_t ror(uint32_t v, uint32_t n) { return (v >> n) | (v << (32 - n)); }

    void compress(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* data, size_t len) {
        total_bits += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            const size_t take = (64 - block_len) < len ? (64 - block_len) : len;
            std::memcpy(block + block_len, data, take);
            block_len += take;
            data += take;
            len -= take;
            if (block_len == 64) {
                compress(block);
                block_len = 0;
            }
        }
    }

    std::string hex() {
        uint8_t pad[72] = {0x80};
        const size_t pad_len = (block_len < 56) ? (56 - block_len) : (120 - block_len);
        update(pad, pad_len);
        total_bits -= static_cast<uint64_t>(pad_len) * 8;  // padding is not message
        uint8_t len_be[8];
        for (int i = 0; i < 8; ++i) {
            len_be[i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xff);
        }
        const uint64_t saved = total_bits;
        update(len_be, 8);
        total_bits = saved;
        char out[65];
        for (int i = 0; i < 8; ++i) {
            std::snprintf(out + i * 8, 9, "%08x", h[i]);
        }
        return std::string(out, 64);
    }
};

std::string sha256Hex(const uint8_t* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.hex();
}

std::map<std::string, std::string> loadHashes(const char* path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in.good()) return out;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        const size_t key_end = text.find('"', pos + 1);
        if (key_end == std::string::npos) break;
        const std::string key = text.substr(pos + 1, key_end - pos - 1);
        const size_t colon = text.find(':', key_end);
        if (colon == std::string::npos) break;
        const size_t val_start = text.find('"', colon);
        if (val_start == std::string::npos) break;
        const size_t val_end = text.find('"', val_start + 1);
        if (val_end == std::string::npos) break;
        out[key] = text.substr(val_start + 1, val_end - val_start - 1);
        pos = val_end + 1;
    }
    return out;
}

bool writeHashes(const char* path, const std::map<std::string, std::string>& hashes) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.good()) return false;
    out << "{\n";
    size_t i = 0;
    for (const auto& kv : hashes) {
        out << "  \"" << kv.first << "\": \"" << kv.second << "\"";
        out << (++i == hashes.size() ? "\n" : ",\n");
    }
    out << "}\n";
    return out.good();
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest = "dng_processor/native/tests/raw_corpus_manifest.json";
    const char* hashes_path = "dng_processor/native/tests/raw_bayer_output_hashes.json";
    bool record_hashes = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--record-hashes") == 0) {
            record_hashes = true;
        } else if (i + 1 < argc && std::strcmp(argv[i], "--manifest") == 0) {
            manifest = argv[++i];
        } else if (i + 1 < argc && std::strcmp(argv[i], "--hashes") == 0) {
            hashes_path = argv[++i];
        }
    }
    // The corpus hash gate is only trustworthy if the digest itself is right; a
    // broken SHA-256 would still produce a self-consistent baseline and compare
    // green forever. Two FIPS 180-4 vectors, one of which crosses the 55-byte
    // padding boundary that a hand-rolled finalizer is most likely to get wrong.
    {
        const char kAbc[] = "abc";
        const char kLong[] =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const std::string got_abc =
            sha256Hex(reinterpret_cast<const uint8_t*>(kAbc), 3);
        const std::string got_empty = sha256Hex(nullptr, 0);
        const std::string got_long =
            sha256Hex(reinterpret_cast<const uint8_t*>(kLong), sizeof(kLong) - 1);
        const bool ok =
            got_abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
            got_empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
            got_long == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
        char detail[240];
        std::snprintf(detail, sizeof(detail), "abc=%s empty_ok=%d long_ok=%d",
                      got_abc.c_str(),
                      got_empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ? 1 : 0,
                      got_long == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" ? 1 : 0);
        report("sha256-selfcheck", nullptr, ok, detail);
    }

    const std::vector<Sample> samples = loadManifest(manifest);
    const std::map<std::string, std::string> baseline = loadHashes(hashes_path);
    std::map<std::string, std::string> recorded;
    std::string first_bayer;
    std::string first_xtrans;
    // Aggregate for the bayer-unchanged gate below: a vacuous PASS would be
    // worse than no line at all, so the count of actually-compared samples is
    // tracked next to the verdict.
    int bayer_hash_compared = 0;
    bool bayer_hash_all_match = true;

    for (const Sample& s : samples) {
        if (s.id.rfind("malformed_", 0) == 0) continue;   // Task 13 owns these
        // expect_route "frontend_only" entries are explicitly NOT router corpus
        // samples (see the manifest note on frontend_switch_lossless_dng): they
        // exist to exercise the frontend's forced-backend switch, and the router
        // still sends them to the DNG SDK route. Driving them through the
        // generic-route expectations below would assert a contradiction.
        if (s.route == "frontend_only") {
            std::printf("[RawE2E] SKIP %s (frontend_only, not a router corpus sample)\n",
                        s.id.c_str());
            continue;
        }
        if (!fileExists(s.path)) {
            std::printf("[RawE2E] SKIP %s (missing file)\n", s.id.c_str());
            continue;
        }

        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        develop.output_space = kRawOutputColorSpaceSrgb;

        RawPipelineResult result;
        const RawErrorCode rc =
            raw_pipeline_decode_file(s.path.c_str(), develop, result);
        ++checked;
        char detail[320];

        if (s.expect_layout == "bayer2x2" && s.expect_error == "kRawSuccess") {
            const bool alpha_ok = rc == kRawSuccess && result.rgba_ptr &&
                alphaAll255(result.rgba_ptr,
                            static_cast<size_t>(result.width) * result.height);
            std::snprintf(detail, sizeof(detail),
                          "size=%ux%u alpha=%s backend=%s repack=%lld rc=%s",
                          result.width, result.height, alpha_ok ? "255" : "BAD",
                          raw_backend_name(result.diag.unpack_backend),
                          static_cast<long long>(result.diag.raw_repack_bytes),
                          raw_error_name(rc));
            report("", s.id.c_str(),
                   alpha_ok && result.width > 0 && result.height > 0 &&
                       result.diag.raw_repack_bytes == 0,
                   detail);

            // Every spec section 6.5 field must be present and non-placeholder.
            const RawDecodeDiagnostics& d = result.diag;
            std::snprintf(detail, sizeof(detail),
                "frontend=%s unpack_backend=%s rawspeed_flags=%u "
                "rawspeed_warning_bits=%u sample_model=%d cfa_repeat=%ux%u "
                "gpu_backend=%s raw_unpack_ms=%.2f gpu_process_ms=%.2f total_ms=%.2f",
                raw_frontend_name(d.frontend), raw_backend_name(d.unpack_backend),
                d.rawspeed_flags, d.rawspeed_warning_bits,
                static_cast<int>(d.sample_model), d.cfa_repeat_width,
                d.cfa_repeat_height, raw_gpu_backend_name(d.gpu_backend),
                d.raw_unpack_ms, d.gpu_process_ms, d.total_ms);
            report("observability", s.id.c_str(),
                   d.frontend == kRawFrontendLibRaw &&
                       d.unpack_backend != kRawDecoderBackendUnknown &&
                       d.sample_model == kRawSampleModelCfa &&
                       d.cfa_repeat_width == 2 && d.cfa_repeat_height == 2 &&
                       d.gpu_backend != kRawGpuBackendNone &&
                       d.raw_unpack_ms > 0.0 && d.gpu_process_ms > 0.0 &&
                       d.total_ms >= d.raw_unpack_ms,
                   detail);

            // Byte-level no-drift record for Task 12's X-Trans wiring.
            if (rc == kRawSuccess && result.rgba_ptr && result.rgba_size > 0) {
                const std::string digest =
                    sha256Hex(result.rgba_ptr, result.rgba_size);
                recorded[s.id] = digest;
                const auto it = baseline.find(s.id);
                if (record_hashes) {
                    std::printf("[RawE2E] hash-record %s sha256=%s\n",
                                s.id.c_str(), digest.c_str());
                } else if (it == baseline.end()) {
                    std::printf("[RawE2E] hash-baseline %s absent (run "
                                "--record-hashes to seed) sha256=%s\n",
                                s.id.c_str(), digest.c_str());
                } else {
                    std::snprintf(detail, sizeof(detail), "sha256=%s want=%s",
                                  digest.c_str(), it->second.c_str());
                    report("hash", s.id.c_str(), digest == it->second, detail);
                    ++bayer_hash_compared;
                    if (digest != it->second) bayer_hash_all_match = false;
                }
            }

            if (first_bayer.empty()) first_bayer = s.path;
        } else if (s.expect_layout == "xtrans6x6" && s.expect_error == "kRawSuccess") {
            const bool alpha_ok = rc == kRawSuccess && result.rgba_ptr &&
                alphaAll255(result.rgba_ptr,
                            static_cast<size_t>(result.width) * result.height);
            std::snprintf(detail, sizeof(detail),
                          "class=xtrans6x6 size=%ux%u alpha=%s backend=%s repack=%lld rc=%s",
                          result.width, result.height, alpha_ok ? "255" : "BAD",
                          raw_backend_name(result.diag.unpack_backend),
                          static_cast<long long>(result.diag.raw_repack_bytes),
                          raw_error_name(rc));
            report("", s.id.c_str(),
                   alpha_ok && result.diag.cfa_repeat_width == 6 &&
                       result.diag.cfa_repeat_height == 6 &&
                       result.diag.raw_repack_bytes == 0,
                   detail);
            if (first_xtrans.empty()) first_xtrans = s.path;
        } else if (s.expect_layout == "xtrans6x6") {
            std::snprintf(detail, sizeof(detail), "error=%s want=%s rgba=%s",
                          raw_error_name(rc), s.expect_error.c_str(),
                          result.rgba_ptr ? "NON-NULL" : "null");
            report("xtrans-expected-failure", s.id.c_str(),
                   rc != kRawSuccess && result.rgba_ptr == nullptr, detail);
        } else {
            std::snprintf(detail, sizeof(detail), "error=%s want=%s",
                          raw_error_name(rc), s.expect_error.c_str());
            report("expected-failure", s.id.c_str(),
                   rc != kRawSuccess && result.rgba_ptr == nullptr, detail);
        }

        if (result.rgba_ptr) dng_rgba_output_release(result.rgba_ptr);
    }

    // Forced native fallback still completes end to end.
    if (!first_bayer.empty()) {
        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        develop.output_space = kRawOutputColorSpaceSrgb;
        RawPipelineResult forced;
        const RawErrorCode rc =
            raw_pipeline_decode_file_forced(first_bayer.c_str(), develop,
                                            RawForcedBackend::kLibRawNative, forced);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "backend=%s size=%ux%u rc=%s",
                      raw_backend_name(forced.diag.unpack_backend),
                      forced.width, forced.height, raw_error_name(rc));
        report("forced-fallback", "first_bayer",
               rc == kRawSuccess && forced.rgba_ptr != nullptr &&
                   forced.diag.unpack_backend == kRawDecoderBackendLibRawNative,
               detail);
        if (forced.rgba_ptr) dng_rgba_output_release(forced.rgba_ptr);
    }

    // The DNG route must still work through the new entry point, unchanged.
    {
        DngResult* via_raw = raw_decode_and_process("image_samples/lossless_dng_sample.dng", 0);
        DngResult* via_dng = dng_decode_and_process("image_samples/lossless_dng_sample.dng");
        char detail[200];
        const bool ok = via_raw && via_dng && via_raw->error_code == 0 &&
                        via_dng->error_code == 0 &&
                        via_raw->width == via_dng->width &&
                        via_raw->height == via_dng->height;
        std::snprintf(detail, sizeof(detail), "raw=%dx%d dng=%dx%d",
                      via_raw ? via_raw->width : -1, via_raw ? via_raw->height : -1,
                      via_dng ? via_dng->width : -1, via_dng ? via_dng->height : -1);
        report("dng-delegation", nullptr, ok, detail);
        if (via_raw) dng_free_result(via_raw);
        if (via_dng) dng_free_result(via_dng);
    }

    // Forced native fallback on X-Trans too.
    if (!first_xtrans.empty()) {
        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        develop.output_space = kRawOutputColorSpaceSrgb;
        RawPipelineResult forced;
        const RawErrorCode rc =
            raw_pipeline_decode_file_forced(first_xtrans.c_str(), develop,
                                            RawForcedBackend::kLibRawNative, forced);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "backend=%s size=%ux%u rc=%s",
                      raw_backend_name(forced.diag.unpack_backend),
                      forced.width, forced.height, raw_error_name(rc));
        report("xtrans-forced-fallback", "first_xtrans",
               rc == kRawSuccess && forced.rgba_ptr != nullptr &&
                   forced.diag.unpack_backend == kRawDecoderBackendLibRawNative,
               detail);
        if (forced.rgba_ptr) dng_rgba_output_release(forced.rgba_ptr);
    } else {
        std::printf("[RawE2E] SKIP xtrans-forced-fallback (no X-Trans sample "
                    "file present)\n");
    }

    // Every non-production class must route to kRawErrLayoutUnsupported, and
    // none of them may reach a kernel. Synthetic inputs, so this runs even when
    // no exotic sample file is on disk.
    {
        struct Case { const char* name; RawSampleModel model; uint32_t comps;
                      uint32_t repeat_w; uint32_t repeat_h; };
        const Case cases[] = {
            {"monochrome",   kRawSampleModelMonochrome,   1, 0, 0},
            {"linear_rgb",   kRawSampleModelLinearRgb,    3, 0, 0},
            {"linear_ycbcr", kRawSampleModelLinearYCbCr,  3, 0, 0},
            {"other_cfa",    kRawSampleModelCfa,          1, 4, 4},
            {"layered",      kRawSampleModelLayered,      3, 0, 0},
            {"multi_frame",  kRawSampleModelMultiFrame,   1, 0, 0},
        };

        static uint16_t storage[64 * 48];
        static RawColorKey quad[16];
        for (int i = 0; i < 16; ++i) {
            const int qr = (i / 4) / 2, qc = (i % 4) / 2;
            quad[i] = (qr == 0 && qc == 0) ? kRawColorKeyRed
                    : (qr == 1 && qc == 1) ? kRawColorKeyBlue : kRawColorKeyGreen;
        }

        for (const Case& kase : cases) {
            RawPlaneView plane{};
            plane.data = storage;
            plane.byte_size = sizeof(storage);
            plane.width = 64; plane.height = 48;
            plane.row_stride_bytes = 128; plane.pixel_stride_bytes = 2;

            RawGpuInput in{};
            in.planes = &plane; in.plane_count = 1;
            in.layout.sample_model = kase.model;
            in.layout.sample_type = kRawSampleTypeU16;
            in.layout.plane_count = 1;
            in.layout.components_per_pixel = kase.comps;
            in.layout.cfa_repeat_width = kase.repeat_w;
            in.layout.cfa_repeat_height = kase.repeat_h;
            in.layout.cfa_pattern = kase.repeat_w ? quad : nullptr;
            in.layout.cfa_pattern_count = kase.repeat_w ? 16 : 0;
            in.active_area = RawRect{0, 0, 64, 48};
            in.default_crop = RawRect{0, 0, 64, 48};
            in.orientation = kRawOrientationTopLeft;
            in.black.repeat_width = 1; in.black.repeat_height = 1;
            in.black.values[0] = 512.0f;
            for (int i = 0; i < 4; ++i) {
                in.white_level[i] = 16383.0f;
                in.as_shot_neutral[i] = 1.0f;
            }
            in.camera_to_pcs.valid = 1;
            in.camera_to_pcs.out_rows = 3; in.camera_to_pcs.in_cols = 3;
            for (int i = 0; i < 9; ++i) in.camera_to_pcs.m[i] = (i % 4 == 0) ? 1.0f : 0.0f;

            RawDevelopParams develop{};
            develop.tone_curve_strength = 1.0f;
            develop.output_space = kRawOutputColorSpaceSrgb;
            RawPipelineResult out;
            const RawErrorCode rc = raw_pipeline_decode_to_rgba(in, develop, out);

            char detail[200];
            std::snprintf(detail, sizeof(detail), "class=%s error=%s rgba=%s",
                          kase.name, raw_error_name(rc),
                          out.rgba_ptr ? "NON-NULL" : "null");
            report("layout-routing", nullptr,
                   rc == kRawErrLayoutUnsupported && out.rgba_ptr == nullptr, detail);
            if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);
        }
    }

    // Synthetic X-Trans product route. No RAF file exists in this checkout, so
    // without this case the X-Trans branch would ship with zero runtime
    // coverage: this is the case that is RED before the branch is wired (the
    // dispatch returned kRawErrLayoutUnsupported for this class) and GREEN
    // after. The tile is the canonical Fujifilm arrangement, i.e. what
    // raw_classify_layout accepts as kRawLayoutClassXTrans6x6.
    {
        const int kW = 72, kH = 48;            // both multiples of the 6x6 tile
        static uint16_t storage[kW * kH];
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                storage[y * kW + x] =
                    static_cast<uint16_t>(600 + ((x * 37 + y * 91) % 9000));
            }
        }
        // Same tile as src/raw_contract_validate.cpp's kCanonicalXTrans
        // (R=0, G=1, B=2), spelled with the contract's enum.
        const int tile[36] = {
            1, 1, 0, 1, 1, 2,
            1, 1, 2, 1, 1, 0,
            2, 0, 1, 0, 2, 1,
            1, 1, 2, 1, 1, 0,
            1, 1, 0, 1, 1, 2,
            0, 2, 1, 2, 0, 1,
        };
        static RawColorKey cfa[36];
        for (int i = 0; i < 36; ++i) cfa[i] = static_cast<RawColorKey>(tile[i]);

        RawPlaneView plane{};
        plane.data = storage;
        plane.byte_size = sizeof(storage);
        plane.width = kW; plane.height = kH;
        plane.row_stride_bytes = kW * 2; plane.pixel_stride_bytes = 2;

        RawGpuInput in{};
        in.planes = &plane; in.plane_count = 1;
        in.layout.sample_model = kRawSampleModelCfa;
        in.layout.sample_type = kRawSampleTypeU16;
        in.layout.plane_count = 1;
        in.layout.components_per_pixel = 1;
        in.layout.cfa_repeat_width = 6;
        in.layout.cfa_repeat_height = 6;
        in.layout.cfa_pattern = cfa;
        in.layout.cfa_pattern_count = 36;
        in.active_area = RawRect{0, 0, kW, kH};
        in.default_crop = RawRect{0, 0, kW, kH};
        in.orientation = kRawOrientationTopLeft;
        in.black.repeat_width = 1; in.black.repeat_height = 1;
        in.black.values[0] = 512.0f;
        for (int i = 0; i < 4; ++i) {
            in.white_level[i] = 16383.0f;
            in.as_shot_neutral[i] = 1.0f;
        }
        in.camera_to_pcs.valid = 1;
        in.camera_to_pcs.out_rows = 3; in.camera_to_pcs.in_cols = 3;
        for (int i = 0; i < 9; ++i) in.camera_to_pcs.m[i] = (i % 4 == 0) ? 1.0f : 0.0f;

        RawDevelopParams develop{};
        develop.tone_curve_strength = 1.0f;
        develop.output_space = kRawOutputColorSpaceSrgb;
        RawPipelineResult out;
        const RawErrorCode rc = raw_pipeline_decode_to_rgba(in, develop, out);

        const bool alpha_ok = rc == kRawSuccess && out.rgba_ptr &&
            alphaAll255(out.rgba_ptr,
                        static_cast<size_t>(out.width) * out.height);
        // A uniform frame would also satisfy alpha/size, so the output is
        // additionally required to carry more than one distinct RGB value.
        bool varies = false;
        if (rc == kRawSuccess && out.rgba_ptr && out.rgba_size >= 8) {
            for (size_t i = 4; i < out.rgba_size && !varies; i += 4) {
                varies = out.rgba_ptr[i] != out.rgba_ptr[0] ||
                         out.rgba_ptr[i + 1] != out.rgba_ptr[1] ||
                         out.rgba_ptr[i + 2] != out.rgba_ptr[2];
            }
        }
        char detail[240];
        std::snprintf(detail, sizeof(detail),
                      "class=xtrans6x6 size=%ux%u alpha=%s varies=%d "
                      "cfa_repeat=%ux%u rc=%s",
                      out.width, out.height, alpha_ok ? "255" : "BAD",
                      varies ? 1 : 0, out.diag.cfa_repeat_width,
                      out.diag.cfa_repeat_height, raw_error_name(rc));
        report("xtrans-synthetic", nullptr,
               alpha_ok && varies && out.width == static_cast<uint32_t>(kW) &&
                   out.height == static_cast<uint32_t>(kH) &&
                   out.diag.cfa_repeat_width == 6 &&
                   out.diag.cfa_repeat_height == 6,
               detail);
        if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);
    }

    // A corrupted X-Trans pattern must fail explicitly, not render garbage.
    if (!first_xtrans.empty()) {
        LibRawFrontendContext ctx;
        if (ctx.open_and_unpack(first_xtrans.c_str()) == kRawSuccess) {
            LibRawGpuInputAdapter adapter;
            RawGpuInput in{};
            RawDevelopParams dev{};
            char reason[256] = {0};
            adapter.build(ctx, &in, &dev, reason, sizeof(reason));

            RawColorKey corrupted[36];
            for (int i = 0; i < 36; ++i) corrupted[i] = in.layout.cfa_pattern[i];
            corrupted[7] = kRawColorKeyUnknown;
            in.layout.cfa_pattern = corrupted;

            RawPipelineResult out;
            const RawErrorCode rc = raw_pipeline_decode_to_rgba(in, dev, out);
            char detail[160];
            std::snprintf(detail, sizeof(detail), "error=%s rgba=%s",
                          raw_error_name(rc), out.rgba_ptr ? "NON-NULL" : "null");
            report("bad-cfa-explicit-failure", nullptr,
                   rc == kRawErrLayoutUnsupported && out.rgba_ptr == nullptr, detail);
            if (out.rgba_ptr) dng_rgba_output_release(out.rgba_ptr);
        }
    } else {
        std::printf("[RawE2E] SKIP bad-cfa-explicit-failure (no X-Trans sample "
                    "file present)\n");
    }

    // The Bayer route must not have moved a single byte. The per-sample `hash`
    // lines above are the evidence; this is the named aggregate verdict.
    if (!record_hashes) {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "compared=%d all_match=%d",
                      bayer_hash_compared, bayer_hash_all_match ? 1 : 0);
        if (bayer_hash_compared == 0) {
            std::printf("[RawE2E] SKIP bayer-unchanged (%s: no Bayer sample "
                        "with a recorded baseline was present)\n", detail);
        } else {
            report("bayer-unchanged", nullptr, bayer_hash_all_match, detail);
        }
    }

    // Nothing may stay checked out, on success or failure.
    {
        char detail[120];
        const size_t rgba_out = dng_debug_pool_checked_out();
        std::snprintf(detail, sizeof(detail), "rgba_checked_out=%zu", rgba_out);
        report("pool-leak", nullptr, rgba_out == 0, detail);
    }

    if (record_hashes) {
        char detail[200];
        const bool wrote = writeHashes(hashes_path, recorded);
        std::snprintf(detail, sizeof(detail), "entries=%zu path=%s",
                      recorded.size(), hashes_path);
        report("hash-file", nullptr, wrote, detail);
    }

    if (checked == 0) {
        std::printf("[RawE2E] FAIL no corpus files were present\n");
        return 1;
    }
    if (failures != 0) {
        std::printf("[RawE2E] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawE2E] ALL PASS\n");
    return 0;
}
