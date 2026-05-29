#include "DngDecoder.h"
#include "HalidePipeline.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

// DNG SDK
#include <dng_camera_profile.h>
#include <dng_color_space.h>
#include <dng_color_spec.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

void printMatrix(const std::string &name, const double *m) {
  if (!m)
    return;
  std::cout << "  " << name << ":\n";
  for (int i = 0; i < 3; i++) {
    std::cout << "    [" << std::setw(10) << m[i * 3 + 0] << ", "
              << std::setw(10) << m[i * 3 + 1] << ", " << std::setw(10)
              << m[i * 3 + 2] << "]\n";
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: test_color_diagnostic <path_to_dng>\n";
    return 1;
  }

  const char *dngPath = argv[1];
  DngDecoder decoder;
  DngMetadata meta;

  std::cout << "============================================================\n";
  std::cout << "  Color Pipeline Diagnostic Tool\n";
  std::cout
      << "============================================================\n\n";

  if (decoder.decodeFile(dngPath, meta) != DngErrorCode::SUCCESS) {
    std::cerr << "Failed to decode DNG\n";
    return 1;
  }

  std::cout << "--- Metadata Inspection ---\n";
  std::cout << "Illuminant1: " << meta.illuminant1 << "\n";
  std::cout << "Illuminant2: " << meta.illuminant2 << "\n";
  std::cout << "AsShotNeutral: " << meta.asShotNeutral[0] << ", "
            << meta.asShotNeutral[1] << ", " << meta.asShotNeutral[2] << "\n";
  std::cout << "AnalogBalance: " << meta.analogBalance[0] << ", "
            << meta.analogBalance[1] << ", " << meta.analogBalance[2] << "\n";

  printMatrix("ColorMatrix1", meta.colorMatrix1);
  printMatrix("ColorMatrix2", meta.colorMatrix2);
  printMatrix("ForwardMatrix1", meta.forwardMatrix);
  printMatrix("ForwardMatrix2", meta.forwardMatrix2);
  printMatrix("CameraCalibration1", meta.cameraCalibration1);
  printMatrix("CameraCalibration2", meta.cameraCalibration2);
  printMatrix("Halide camToSrgb (Our Result)", meta.camToSrgb);

  // DNG SDK Reference Matrix Analysis
  std::cout << "\n--- DNG SDK Internal Matrix Analysis ---\n";
  try {
    dng_host host;
    dng_file_stream stream(dngPath);
    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);
    AutoPtr<dng_negative> neg(host.Make_dng_negative());
    neg->Parse(host, stream, info);
    neg->PostParse(host, stream, info);

    dng_render render(host, *neg);
    render.SetFinalSpace(dng_space_sRGB::Get());

    // Find the first valid camera profile to match what Decoder does
    const dng_camera_profile &profile = neg->ProfileByIndex(0);
    dng_camera_profile_id profileID = profile.ProfileID();

    AutoPtr<dng_color_spec> spec(neg->MakeColorSpec(profileID));

    // Match White Balance setup from DngDecoder.cpp
    if (neg->HasCameraNeutral()) {
      spec->SetWhiteXY(spec->NeutralToXY(neg->CameraNeutral()));
    } else if (neg->HasCameraWhiteXY()) {
      spec->SetWhiteXY(neg->CameraWhiteXY());
    }

    // Internal matrix from SDK
    dng_matrix sdkMatrix =
        dng_space_sRGB::Get().MatrixFromPCS() * spec->CameraToPCS();
    double sdkM[9];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        sdkM[i * 3 + j] = sdkMatrix[i][j];
    printMatrix("DNG SDK camToSrgb (Target Reference)", sdkM);

    // Compute Delta
    std::cout << "Matrix Delta (Our - SDK):\n";
    for (int i = 0; i < 3; i++) {
      std::cout << "    [";
      for (int j = 0; j < 3; j++) {
        if (j)
          std::cout << ", ";
        std::cout << std::setw(10)
                  << (meta.camToSrgb[i * 3 + j] - sdkM[i * 3 + j]);
      }
      std::cout << "]\n";
    }
  } catch (...) {
    std::cerr << "Failed to extract SDK reference matrix\n";
  }

  // Pixel comparison
  std::cout << "\n--- Patch Analysis (Pixel Probing) ---\n";
  int outW, outH;
  uint8_t *halideOutput = HalidePipeline::process(
      decoder.getRawBuffer(), meta.width, meta.height, meta.blackLevel,
      meta.whiteLevel, meta.asShotNeutral, meta.camToSrgb,
      meta.baselineExposure, meta, outW, outH);

  if (!halideOutput) {
    std::cerr << "Halide process failed\n";
    return 1;
  }

  // Ref Render
  dng_host refHost;
  dng_file_stream refStream(dngPath);
  dng_info refInfo;
  refInfo.Parse(refHost, refStream);
  refInfo.PostParse(refHost);
  AutoPtr<dng_negative> refNeg(refHost.Make_dng_negative());
  refNeg->Parse(refHost, refStream, refInfo);
  refNeg->PostParse(refHost, refStream, refInfo);
  refNeg->ReadStage1Image(refHost, refStream, refInfo);
  refNeg->BuildStage2Image(refHost);
  refNeg->BuildStage3Image(refHost);
  dng_render refRender(refHost, *refNeg);
  refRender.SetFinalSpace(dng_space_sRGB::Get());
  refRender.SetFinalPixelType(ttByte);
  refRender.SetMaximumSize(0);
  AutoPtr<dng_image> refImg(refRender.Render());

  if (refImg.Get()) {
    std::vector<uint8_t> refRGB(meta.width * meta.height * 3);
    dng_pixel_buffer rbuf;
    rbuf.fArea = refImg->Bounds();
    rbuf.fPlane = 0;
    rbuf.fPlanes = 3;
    rbuf.fPixelType = ttByte;
    rbuf.fPixelSize = 1;
    rbuf.fData = refRGB.data();
    rbuf.fRowStep = meta.width * 3;
    rbuf.fColStep = 3;
    rbuf.fPlaneStep = 1;
    refImg->Get(rbuf);

    // Sample points (Center and 4 corners)
    int sx[] = {(int)meta.width / 2, 100, (int)meta.width - 100, 100,
                (int)meta.width - 100};
    int sy[] = {(int)meta.height / 2, 100, 100, (int)meta.height - 100,
                (int)meta.height - 100};

    std::cout << std::left << std::setw(15) << "Coordinate" << std::setw(25)
              << "Our RGB" << std::setw(25) << "SDK RGB"
              << "Delta\n";

    for (int i = 0; i < 5; i++) {
      int x = sx[i];
      int y = sy[i];
      size_t hIdx = (size_t)(y * meta.width + x) * 4;
      size_t rIdx = (size_t)(y * meta.width + x) * 3;

      int or_ = halideOutput[hIdx], og = halideOutput[hIdx + 1],
          ob = halideOutput[hIdx + 2];
      int rr_ = refRGB[rIdx], rg = refRGB[rIdx + 1], rb = refRGB[rIdx + 2];

      std::cout << "(" << std::setw(4) << x << "," << std::setw(4) << y << ")  "
                << "[" << std::setw(3) << or_ << "," << std::setw(3) << og
                << "," << std::setw(3) << ob << "]    "
                << "[" << std::setw(3) << rr_ << "," << std::setw(3) << rg
                << "," << std::setw(3) << rb << "]    "
                << "(" << (or_ - rr_) << "," << (og - rg) << "," << (ob - rb)
                << ")\n";
    }
  }

  delete[] halideOutput;
  std::cout << "\nDiagnostic Finished.\n";
  return 0;
}
