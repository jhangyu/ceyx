// Phase 8.1.6 Stage E micro-benchmark:
// Measure ONLY dng_opcode_WarpRectilinear::Apply (SDK ProcessArea) wall time
// on a post-demosaic 3-plane uint16 image. Repeat 5, median + min/max.
// Threading vehicle: ConcurrentDngHost(20).

#include "stage_contract_checks.h"
#include "ConcurrentDngHost.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_lens_correction.h>
#include <dng_mosaic_halide.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>

using namespace std;
using namespace std::chrono;

namespace {

AutoPtr<dng_image> makeImageFromInterleaved(dng_host& host, uint32_t w, uint32_t h,
                                            const vector<uint16_t>& data) {
    AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(dng_point((int32)h, (int32)w)), 3, ttShort));
    dng_pixel_buffer buf;
    buf.fArea = image->Bounds();
    buf.fPlane = 0; buf.fPlanes = 3; buf.fPixelType = ttShort; buf.fPixelSize = sizeof(uint16_t);
    buf.fData = const_cast<uint16_t*>(data.data());
    buf.fRowStep = (int32)(w * 3); buf.fColStep = 3; buf.fPlaneStep = 1;
    image->Put(buf);
    return AutoPtr<dng_image>(image.Release());
}

const dng_opcode_WarpRectilinear* findWarp(const dng_negative& neg) {
    const dng_opcode_list& list = neg.OpcodeList3();
    for (uint32_t i = 0; i < list.Count(); ++i) {
        if (list.Entry(i).OpcodeID() == dngOpcode_WarpRectilinear)
            return &static_cast<const dng_opcode_WarpRectilinear&>(list.Entry(i));
    }
    return nullptr;
}

void extractInterleaved(dng_image* img, vector<uint16_t>& out) {
    const uint32_t w = img->Width(), h = img->Height();
    out.resize((size_t)w * h * img->Planes());
    dng_pixel_buffer buf;
    buf.fArea = img->Bounds();
    buf.fPlane = 0; buf.fPlanes = img->Planes(); buf.fPixelType = ttShort; buf.fPixelSize = sizeof(uint16_t);
    buf.fData = out.data();
    buf.fRowStep = (int32)(w * img->Planes()); buf.fColStep = img->Planes(); buf.fPlaneStep = 1;
    img->Get(buf);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { cerr << "Usage: " << argv[0] << " <dng_path>\n"; return 1; }
    const string dngPath = argv[1];
    constexpr int kRepeat = 5;

    try {
        ConcurrentDngHost host(20);
        cout << "ConcurrentDngHost PerformAreaTaskThreads = " << host.PerformAreaTaskThreads() << "\n";

        dng_file_stream stream(dngPath.c_str());
        dng_info info;
        info.Parse(host, stream); info.PostParse(host);
        if (!info.IsValidDNG()) { cerr << "ERROR: not a valid DNG\n"; return 1; }

        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);

        dng_image* s2 = const_cast<dng_image*>(negative->Stage2Image());
        const uint32_t w = s2->Width(), h = s2->Height();
        vector<uint16_t> stage2((size_t)w * h);
        {
            dng_pixel_buffer buf;
            buf.fArea = s2->Bounds();
            buf.fPlane = 0; buf.fPlanes = 1; buf.fPixelType = ttShort; buf.fPixelSize = sizeof(uint16_t);
            buf.fData = stage2.data();
            buf.fRowStep = (int32)w; buf.fColStep = 1; buf.fPlaneStep = 1;
            s2->Get(buf);
        }

        vector<uint16_t> preWarp((size_t)w * h * 3);
        demosaic_bilinear_compat(stage2.data(), (int)w, (int)h, preWarp.data());
        cout << "Pre-warp image: " << w << "x" << h << " 3p uint16 (bilinear demosaic)\n";

        const dng_opcode_WarpRectilinear* warpOp = findWarp(*negative);
        if (!warpOp) { cerr << "ERROR: no WarpRectilinear in OpcodeList3\n"; return 1; }

        vector<double> times_ms;
        times_ms.reserve(kRepeat);
        for (int i = 0; i < kRepeat; ++i) {
            // Fresh input each iter so we time only the warp Apply (not memcpy quirks).
            AutoPtr<dng_image> img = makeImageFromInterleaved(host, w, h, preWarp);
            const auto t0 = high_resolution_clock::now();
            const_cast<dng_opcode_WarpRectilinear*>(warpOp)->Apply(host, *negative, img);
            const auto t1 = high_resolution_clock::now();
            const double ms = duration_cast<microseconds>(t1 - t0).count() / 1000.0;
            times_ms.push_back(ms);
            cout << "iter " << (i + 1) << ": " << fixed << setprecision(2) << ms << " ms\n";
        }

        vector<double> sorted = times_ms;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        const double mn = sorted.front();
        const double mx = sorted.back();
        cout << "----\n";
        cout << "median: " << fixed << setprecision(2) << median << " ms\n";
        cout << "min:    " << mn << " ms\n";
        cout << "max:    " << mx << " ms\n";
        cout << "decision: " << (median <= 200.0 ? "OPTION_1 (SDK CPU warp)" : "OPTION_2 (tile-streaming Halide)") << "\n";
        return 0;
    } catch (const dng_exception& e) {
        cerr << "DNG Exception: " << e.ErrorCode() << "\n";
    } catch (const std::exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }
    return 1;
}
