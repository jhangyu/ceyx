/*
---
file_summary: "Production C ABI decode harness with contract-first RGB verification."
functions:
  - name: "resolveArtifactDir"
    description: "Restrict binary artifacts to the repository artifacts directory."
    lines: "79-96"
  - name: "validateContract"
    description: "Validate the interleaved RGBA8 FFI result before any RGB comparison."
    lines: "98-142"
  - name: "writeAndCompareRgb"
    description: "Stream RGBA-to-RGB output and optionally compare the Halide test render."
    lines: "144-220"
  - name: "main"
    description: "Parse arguments, invoke the production C ABI, verify, and free results."
    lines: "224-275"
---
*/

#include "dng_ffi_api.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void printUsage(const char *program) {
  std::cerr << "Usage: " << program
            << " <dng_path> <repeat_count> [--save-raw <artifact_dir>]\n";
}

bool parseRepeatCount(const char *text, int *repeatCount) {
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!text[0] || !end || *end != '\0' || value < 1 ||
      value > std::numeric_limits<int>::max()) {
    return false;
  }
  *repeatCount = static_cast<int>(value);
  return true;
}

bool isWithin(const fs::path &child, const fs::path &parent) {
  auto childIt = child.begin();
  for (auto parentIt = parent.begin(); parentIt != parent.end(); ++parentIt) {
    if (childIt == child.end() || *childIt != *parentIt) {
      return false;
    }
    ++childIt;
  }
  return true;
}

fs::path findRepoRoot() {
  fs::path current = fs::current_path();
  while (true) {
    if (fs::exists(current / "rule.md") &&
        fs::exists(current / "dng_processor" / "native")) {
      return current;
    }
    if (current == current.parent_path()) {
      return {};
    }
    current = current.parent_path();
  }
}

bool resolveArtifactDir(const fs::path &requested, fs::path *artifactDir) {
  const fs::path repoRoot = findRepoRoot();
  if (repoRoot.empty()) {
    std::cerr << "ERROR: could not locate repo root from current directory\n";
    return false;
  }

  const fs::path artifactsRoot = fs::weakly_canonical(repoRoot / "artifacts");
  const fs::path resolved = fs::weakly_canonical(fs::absolute(requested));
  if (!isWithin(resolved, artifactsRoot)) {
    std::cerr << "ERROR: --save-raw must stay under " << artifactsRoot << "\n";
    return false;
  }

  fs::create_directories(resolved);
  *artifactDir = resolved;
  return true;
}

bool validateContract(const DngResult *result, size_t *rgbaBytes) {
  if (!result) {
    std::cout << "[Contract] FAIL reason=null_result\n";
    return false;
  }
  if (result->error_code != 0) {
    std::cout << "[Contract] FAIL reason=decode_error err=" << result->error_code
              << "\n";
    return false;
  }
  if (!result->rgba_data) {
    std::cout << "[Contract] FAIL reason=null_rgba_data\n";
    return false;
  }
  if (result->width <= 0 || result->height <= 0) {
    std::cout << "[Contract] FAIL reason=invalid_dimensions w=" << result->width
              << " h=" << result->height << "\n";
    return false;
  }

  const size_t width = static_cast<size_t>(result->width);
  const size_t height = static_cast<size_t>(result->height);
  if (width > std::numeric_limits<size_t>::max() / height ||
      width * height > std::numeric_limits<size_t>::max() / 4) {
    std::cout << "[Contract] FAIL reason=buffer_size_overflow\n";
    return false;
  }

  const size_t pixelCount = width * height;
  *rgbaBytes = pixelCount * 4;
  for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
    if (result->rgba_data[pixel * 4 + 3] != 255) {
      std::cout << "[Contract] FAIL reason=alpha_not_255 pixel=" << pixel
                << "\n";
      return false;
    }
  }

  std::cout << "[Contract] PASS"
            << " w=" << result->width << " h=" << result->height
            << " planes=4 pixelType=uint8 pixelSize=1"
            << " buffer_size=" << *rgbaBytes
            << " layout=interleaved_RGBA8\n";
  return true;
}

bool writeAndCompareRgb(const DngResult &result, const fs::path &artifactDir,
                        const std::string &prefix) {
  const size_t pixelCount =
      static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
  const size_t rgbBytes = pixelCount * 3;
  const fs::path output =
      artifactDir / (prefix == "lossless_dng_sample" ? "lossless_ffi_render.raw"
                                                     : "lossy_ffi_render.raw");
  const fs::path expected =
      artifactDir / (prefix == "lossless_dng_sample" ? "lossless_test_render.raw"
                                                     : "lossy_test_render.raw");

  std::ofstream rgbOut(output, std::ios::binary);
  if (!rgbOut) {
    std::cerr << "ERROR: could not write FFI RGB artifact: " << output << "\n";
    return false;
  }

  const bool hasExpected = fs::exists(expected);
  std::ifstream testRender;
  if (hasExpected) {
    if (fs::file_size(expected) != rgbBytes) {
      std::cout << "[FFI RGB MATCH] render: byte_exact=0 psnr=0.00 dB [FAIL]"
                << " reason=size_mismatch\n";
      return false;
    }
    testRender.open(expected, std::ios::binary);
    if (!testRender) {
      std::cerr << "ERROR: could not read Halide test render: " << expected
                << "\n";
      return false;
    }
  }

  bool byteExact = true;
  long double squaredError = 0.0;
  for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
    const char rgb[3] = {
        static_cast<char>(result.rgba_data[pixel * 4 + 0]),
        static_cast<char>(result.rgba_data[pixel * 4 + 1]),
        static_cast<char>(result.rgba_data[pixel * 4 + 2]),
    };
    rgbOut.write(rgb, sizeof(rgb));
    if (hasExpected) {
      char reference[3];
      testRender.read(reference, sizeof(reference));
      for (size_t channel = 0; channel < 3; ++channel) {
        const int actual = static_cast<unsigned char>(rgb[channel]);
        const int expectedValue =
            static_cast<unsigned char>(reference[channel]);
        const int difference = actual - expectedValue;
        squaredError += static_cast<long double>(difference * difference);
        byteExact = byteExact && difference == 0;
      }
    }
  }

  if (!rgbOut) {
    std::cerr << "ERROR: failed while writing FFI RGB artifact: " << output
              << "\n";
    return false;
  }
  std::cout << "[FFI RGB ARTIFACT] " << output << " bytes=" << rgbBytes
            << "\n";

  if (!hasExpected) {
    std::cout << "[FFI RGB MATCH] render: SKIP test_render_missing=" << expected
              << "\n";
    return true;
  }

  const double mse = static_cast<double>(squaredError / rgbBytes);
  const double psnr =
      byteExact ? 999.0 : 10.0 * std::log10((255.0 * 255.0) / mse);
  std::cout << "[FFI RGB MATCH] render: byte_exact=" << (byteExact ? 1 : 0)
            << " psnr=" << std::fixed << std::setprecision(2) << psnr
            << " dB [" << (byteExact ? "PASS" : "FAIL") << "]\n";
  return byteExact;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    printUsage(argv[0]);
    return 2;
  }

  int repeatCount = 0;
  if (!parseRepeatCount(argv[2], &repeatCount)) {
    printUsage(argv[0]);
    return 2;
  }

  fs::path artifactDir;
  const bool saveRaw = argc == 5;
  if (saveRaw) {
    if (std::string(argv[3]) != "--save-raw" ||
        !resolveArtifactDir(argv[4], &artifactDir)) {
      printUsage(argv[0]);
      return 2;
    }
  }

  const std::string prefix = fs::path(argv[1]).stem().string();

  // T5b: opt-in warmup hook so the "warmup-then-first-decode" cold tax can be
  // measured. Off by default (env unset) and does not alter the decode path's
  // numeric output — it only primes the GPU pipelines before the loop, exactly
  // as the Flutter app does at startup. Enable with DNG_HARNESS_WARMUP=1.
  if (const char *warmupEnv = std::getenv("DNG_HARNESS_WARMUP");
      warmupEnv && warmupEnv[0] && warmupEnv[0] != '0') {
    const auto warmStart = Clock::now();
    const int32_t warmRc = dng_decoder_warmup_for_size(6000, 4000);
    const double warmMs =
        std::chrono::duration<double, std::milli>(Clock::now() - warmStart)
            .count();
    std::cerr << "[Harness] warmup_for_size(6000,4000) rc=" << warmRc
              << " warmup_ms=" << std::fixed << std::setprecision(2) << warmMs
              << "\n";
  }

  for (int run = 1; run <= repeatCount; ++run) {
    const auto start = Clock::now();
    DngResult *result = dng_decode_and_process(argv[1]);
    const auto end = Clock::now();

    size_t rgbaBytes = 0;
    if (!validateContract(result, &rgbaBytes)) {
      dng_free_result(result);
      return 1;
    }

    const double wallMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "[FFI run " << run << "] ok=1"
              << " w=" << result->width << " h=" << result->height
              << " rgb_bytes=" << (rgbaBytes / 4 * 3) << " decode_ms="
              << std::fixed << std::setprecision(2) << result->decode_ms
              << " process_ms=" << result->process_ms << " wall_ms=" << wallMs
              << " err=" << result->error_code << "\n";

    if (saveRaw && !writeAndCompareRgb(*result, artifactDir, prefix)) {
      dng_free_result(result);
      return 1;
    }
    dng_free_result(result);

    // W5-#15 + 7.1: leak assertion — after freeing the result, BOTH the RGBA
    // and RGB output pools must have zero checked-out buffers. A non-zero
    // count means a checkout leaked (RAII guard or free path failed to return
    // the buffer). The RGB pool is exercised when DNG_FUSE_RGBA=0.
    const size_t rgbaLeaked = dng_debug_pool_checked_out();
    const size_t rgbLeaked = dng_debug_rgb_pool_checked_out();
    if (rgbaLeaked != 0 || rgbLeaked != 0) {
      std::cout << "[Pool] FAIL rgba_checked_out=" << rgbaLeaked
                << " rgb_checked_out=" << rgbLeaked
                << " (expected 0 after free)\n";
      return 1;
    }
  }

  std::cout << "[Pool] PASS rgba_checked_out=0 rgb_checked_out=0 after "
            << repeatCount << " run(s)\n";
  return 0;
}
