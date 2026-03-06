with open("src/HalidePipeline.cpp", "r") as f:
    lines = f.readlines()

new_lines = lines[:412] # Up to line 412: `} // anonymous namespace`
with open("src/HalidePipeline.cpp", "w") as f:
    f.writelines(new_lines)
    f.write("\n")
    f.write("""// ============================================================================
// Main pipeline entry (AOT)
// ============================================================================
uint8_t *HalidePipeline::process(
    const uint16_t *bayerData, int width, int height, uint32_t blackLevel,
    uint32_t whiteLevel, const double asShotNeutral[3],
    const double camToSrgb[9], double baselineExposure,
    const DngMetadata &metadata, int &outWidth, int &outHeight) {

  outWidth = width;
  outHeight = height;

  auto t_total_start = std::chrono::steady_clock::now();

  const bool hasHSM =
      metadata.hsmHueDivisions > 0 && metadata.hsmSatDivisions > 1;
  const bool hasLT = metadata.ltHueDivisions > 0 && metadata.ltSatDivisions > 1;

  std::cerr << "[Halide AOT] Processing " << width << "x" << height
            << " BaselineExposure=" << baselineExposure << " EV"
            << " HueSatMap=" << (hasHSM ? "YES" : "NO")
            << " LookTable=" << (hasLT ? "YES" : "NO") << "\\n";

  auto t_bind_start = std::chrono::steady_clock::now();

  float range = static_cast<float>(whiteLevel - blackLevel);
  float bl = static_cast<float>(blackLevel);
  Halide::Runtime::Buffer<uint16_t> bayerBuf(const_cast<uint16_t *>(bayerData), width, height);
  bayerBuf.set_host_dirty();
  float expGain = (float)std::pow(2.0, baselineExposure);

  Halide::Runtime::Buffer<float> hsmBuf(3, std::max((int)metadata.hsmSatDivisions, 1),
                                        std::max((int)metadata.hsmHueDivisions, 1),
                                        std::max((int)metadata.hsmValDivisions, 1));
  if (hasHSM) {
    for (int v = 0; v < metadata.hsmValDivisions; v++) {
      for (int h = 0; h < metadata.hsmHueDivisions; h++) {
        for (int s = 0; s < metadata.hsmSatDivisions; s++) {
          int idx = v * metadata.hsmHueDivisions * metadata.hsmSatDivisions +
                    h * metadata.hsmSatDivisions + s;
          hsmBuf(0, s, h, v) = metadata.hsmData[idx].hueShift;
          hsmBuf(1, s, h, v) = metadata.hsmData[idx].satScale;
          hsmBuf(2, s, h, v) = metadata.hsmData[idx].valScale;
        }
      }
    }
  }
  hsmBuf.set_host_dirty();

  Halide::Runtime::Buffer<float> ltBuf(3, std::max((int)metadata.ltSatDivisions, 1),
                                       std::max((int)metadata.ltHueDivisions, 1),
                                       std::max((int)metadata.ltValDivisions, 1));
  if (hasLT) {
    for (int v = 0; v < metadata.ltValDivisions; v++) {
      for (int h = 0; h < metadata.ltHueDivisions; h++) {
        for (int s = 0; s < metadata.ltSatDivisions; s++) {
          int idx = v * metadata.ltHueDivisions * metadata.ltSatDivisions +
                    h * metadata.ltSatDivisions + s;
          ltBuf(0, s, h, v) = metadata.ltData[idx].hueShift;
          ltBuf(1, s, h, v) = metadata.ltData[idx].satScale;
          ltBuf(2, s, h, v) = metadata.ltData[idx].valScale;
        }
      }
    }
  }
  ltBuf.set_host_dirty();

  const double *tcPts =
      (metadata.toneCurveCount > 0) ? metadata.toneCurvePoints : nullptr;
  const int tcCount =
      (metadata.toneCurveCount > 0) ? (int)metadata.toneCurveCount : 0;
  bool hasTC = true;

  Halide::Runtime::Buffer<float> tcBuf(4096);
  for (int i = 0; i < 4096; i++) {
    float x = (float)i / 4095.0f;
    if (tcCount > 0 && tcPts) {
      tcBuf(i) =
          std::min(std::max(evalToneCurve(tcPts, tcCount, x), 0.0f), 1.0f);
    } else {
      tcBuf(i) = evalAcrLUT(x);
    }
  }
  tcBuf.set_host_dirty();

  const bool hasLR = metadata.lrParams.parsed;
  const float lrExpGainVal =
      hasLR ? static_cast<float>(std::pow(2.0, metadata.lrParams.exposure2012))
            : 1.0f;
  const float lrContrastVal =
      hasLR ? static_cast<float>(metadata.lrParams.contrast2012 / 100.0) : 0.0f;
  const float lrSatVal =
      hasLR ? static_cast<float>(metadata.lrParams.saturation / 100.0) : 0.0f;
  const float lrVibVal =
      hasLR ? static_cast<float>(metadata.lrParams.vibrance / 100.0) : 0.0f;

  auto t_bind_end = std::chrono::steady_clock::now();
  double bindMs =
      std::chrono::duration<double, std::milli>(t_bind_end - t_bind_start)
          .count();
  std::cerr << "[Halide Perf 7.1] bind_params (buffers): " << bindMs << " ms\\n";

  auto t_alloc_start = std::chrono::steady_clock::now();

  uint8_t *out = new (std::nothrow) uint8_t[(size_t)width * height * 4];
  if (!out)
    return nullptr;

  for (int i = 0; i < width * height; i++) {
    out[i * 4 + 3] = 255;
  }

  auto t_alloc_end = std::chrono::steady_clock::now();
  double allocMs =
      std::chrono::duration<double, std::milli>(t_alloc_end - t_alloc_start)
          .count();
  std::cerr << "[Halide Perf 7.1] alloc+alpha_fill: " << allocMs << " ms\\n";

  auto t_buf_start = std::chrono::steady_clock::now();

  Halide::Runtime::Buffer<uint8_t> halide_out =
      Halide::Runtime::Buffer<uint8_t>::make_interleaved(out, width, height, 3);
  halide_out.raw_buffer()->dim[0].stride = 4;
  halide_out.raw_buffer()->dim[1].stride = width * 4;
  halide_out.raw_buffer()->dim[2].stride = 1;

  auto t_buf_end = std::chrono::steady_clock::now();
  double bufMs =
      std::chrono::duration<double, std::milli>(t_buf_end - t_buf_start)
          .count();
  std::cerr << "[Halide Perf 7.1] buffer_setup: " << bufMs << " ms\\n";

  auto t_realize_start = std::chrono::steady_clock::now();

  int result = dng_pipeline(
      bayerBuf.raw_buffer(),
      bl, range,
      (float)camToSrgb[0], (float)camToSrgb[1], (float)camToSrgb[2],
      (float)camToSrgb[3], (float)camToSrgb[4], (float)camToSrgb[5],
      (float)camToSrgb[6], (float)camToSrgb[7], (float)camToSrgb[8],
      expGain,
      hasHSM, hasLT, hasTC, hasLR,
      hsmBuf.raw_buffer(), ltBuf.raw_buffer(), tcBuf.raw_buffer(),
      std::max((int)metadata.hsmHueDivisions, 1),
      std::max((int)metadata.hsmSatDivisions, 1),
      std::max((int)metadata.hsmValDivisions, 1),
      std::max((int)metadata.ltHueDivisions, 1),
      std::max((int)metadata.ltSatDivisions, 1),
      std::max((int)metadata.ltValDivisions, 1),
      lrExpGainVal, 1.0f + lrContrastVal, lrSatVal, lrVibVal,
      halide_out.raw_buffer()
  );

  if (result != 0) {
      std::cerr << "[Halide AOT] ERROR in dng_pipeline: " << result << "\\n";
      delete[] out;
      return nullptr;
  }

  auto t_copy_start = std::chrono::steady_clock::now();
  halide_out.copy_to_host();
  auto t_copy_end = std::chrono::steady_clock::now();
  double copyMs =
      std::chrono::duration<double, std::milli>(t_copy_end - t_copy_start)
          .count();
  std::cerr << "[Halide Perf 7.1] copy_to_host (GPU→CPU): " << copyMs << " ms\\n";

  auto t_realize_end = std::chrono::steady_clock::now();
  double realizeMs =
      std::chrono::duration<double, std::milli>(t_realize_end - t_realize_start)
          .count();
  std::cerr << "[Halide Perf 7.1] AOT execution (incl copy_to_host): " << realizeMs
            << " ms\\n";

  auto t_total_end = std::chrono::steady_clock::now();
  double totalMs =
      std::chrono::duration<double, std::milli>(t_total_end - t_total_start)
          .count();
  std::cerr << "[Halide Perf 7.1] === TOTAL process(): " << totalMs << " ms ===\\n";

  return out;
}
""")
