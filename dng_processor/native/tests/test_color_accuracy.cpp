// Visual Regression Test: Render DNG via Adobe SDK full pipeline,
// export raw RGB, then compare with sample.jpg using Python PSNR/SSIM.
//
// Usage: test_color_accuracy <dng_path> <output_rgb_path>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include <dng_color_space.h>
#include <dng_exceptions.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <dng_path> <output_rgb_path>\n";
    return 1;
  }

  const std::string dngPath = argv[1];
  const std::string outPath = argv[2];

  try {
    dng_host host;

    std::cerr << "[RENDER] Opening " << dngPath << "...\n";
    dng_file_stream stream(dngPath.c_str());

    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);

    if (!info.IsValidDNG()) {
      std::cerr << "[RENDER] Not a valid DNG!\n";
      return 1;
    }

    // Parse the negative
    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    negative->Parse(host, stream, info);
    negative->PostParse(host, stream, info);

    // Read stage 1 (raw)
    std::cerr << "[RENDER] ReadStage1Image...\n";
    negative->ReadStage1Image(host, stream, info);

    // Build stage 2 (linearized)
    std::cerr << "[RENDER] BuildStage2Image...\n";
    negative->BuildStage2Image(host);

    // Build stage 3 (demosaiced)
    std::cerr << "[RENDER] BuildStage3Image...\n";
    negative->BuildStage3Image(host);

    // Render to final sRGB 8-bit
    std::cerr << "[RENDER] Rendering to sRGB...\n";
    dng_render render(host, *negative);
    render.SetFinalSpace(dng_space_sRGB::Get());
    render.SetFinalPixelType(ttByte);
    render.SetMaximumSize(0); // full resolution

    auto t0 = std::chrono::steady_clock::now();
    AutoPtr<dng_image> finalImage(render.Render());
    auto t1 = std::chrono::steady_clock::now();
    double renderMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!finalImage.Get()) {
      std::cerr << "[RENDER] Render returned null!\n";
      return 1;
    }

    uint32 w = finalImage->Width();
    uint32 h = finalImage->Height();
    uint32 planes = finalImage->Planes();
    std::cerr << "[RENDER] Final image: " << w << "x" << h
              << " planes=" << planes << " render_time=" << renderMs << "ms\n";

    // Extract pixels as interleaved RGB (uint8)
    std::vector<uint8_t> rgbData(static_cast<size_t>(w) * h * 3);
    dng_pixel_buffer buf;
    buf.fArea = finalImage->Bounds();
    buf.fPlane = 0;
    buf.fPlanes = 3;
    buf.fPixelType = ttByte;
    buf.fPixelSize = 1;
    buf.fData = rgbData.data();
    buf.fRowStep = w * 3;
    buf.fColStep = 3;
    buf.fPlaneStep = 1;
    finalImage->Get(buf);

    // Write header: width(4B) + height(4B) + RGB data
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs) {
      std::cerr << "[RENDER] Failed to open output: " << outPath << "\n";
      return 1;
    }
    ofs.write(reinterpret_cast<const char *>(&w), 4);
    ofs.write(reinterpret_cast<const char *>(&h), 4);
    ofs.write(reinterpret_cast<const char *>(rgbData.data()), rgbData.size());
    ofs.close();

    std::cerr << "[RENDER] Wrote " << rgbData.size() + 8 << " bytes to "
              << outPath << "\n";
    std::cout << "RENDER_OK " << w << " " << h << " " << renderMs << "\n";
    return 0;

  } catch (const dng_exception &e) {
    std::cerr << "[RENDER] DNG Exception: " << e.ErrorCode() << "\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "[RENDER] Std Exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "[RENDER] Unknown exception\n";
    return 1;
  }
}
