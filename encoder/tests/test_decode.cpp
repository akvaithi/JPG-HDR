// Round-trips our own bitstream through libjpeg. Only built when libjpeg is
// available; it is a build-time check, never a runtime dependency.
#include <cstddef>
#include <cstdio>

#include <jpeglib.h>

#include <cmath>
#include <csetjmp>
#include <cstring>
#include <string>

#include "color.h"
#include "encoder.h"
#include "jpeg_encoder.h"
#include "test_support.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

struct ErrorManager {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX] = {0};
};

void onError(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

struct Decoded {
  bool ok = false;
  std::string error;
  uint32_t width = 0, height = 0;
  int components = 0;
  std::vector<uint8_t> pixels;
};

Decoded decode(const Bytes& jpeg) {
  Decoded out;
  jpeg_decompress_struct cinfo;
  ErrorManager err;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = onError;
  if (setjmp(err.jump)) {
    out.error = err.message;
    jpeg_destroy_decompress(&cinfo);
    return out;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg.data(), static_cast<unsigned long>(jpeg.size()));
  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);
  out.width = cinfo.output_width;
  out.height = cinfo.output_height;
  out.components = cinfo.output_components;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height *
                    out.components);
  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t* row = out.pixels.data() + static_cast<size_t>(cinfo.output_scanline) *
                                           out.width * out.components;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  out.ok = true;
  return out;
}

void decodesOurOutput(uint32_t w, uint32_t h, int channels, int quality,
                      bool subsample, bool optimize) {
  std::vector<uint8_t> src(static_cast<size_t>(w) * h * channels);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      for (int c = 0; c < channels; ++c) {
        // A smooth pattern: high-quality JPEG should reproduce it closely.
        double v = 128.0 + 100.0 * std::sin(x * 0.05 + c) * std::cos(y * 0.04);
        src[(static_cast<size_t>(y) * w + x) * channels + c] =
            static_cast<uint8_t>(std::min(255.0, std::max(0.0, v)));
      }

  JpegOptions o;
  o.quality = quality;
  o.chromaSubsample = subsample;
  o.optimizeHuffman = optimize;
  JpegImage img{w, h, channels, src.data()};
  Bytes encoded = encodeJpeg(img, o);

  Decoded d = decode(encoded);
  if (!d.ok) {
    reportFailure(__FILE__, __LINE__, "libjpeg rejected our output: " + d.error);
    return;
  }
  CHECK_EQ(d.width, w);
  CHECK_EQ(d.height, h);
  CHECK_EQ(d.components, channels);

  // Compare the luma-ish channel: at q95 4:4:4 the error must be small.
  double sumSq = 0;
  size_t n = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      int a = src[(static_cast<size_t>(y) * w + x) * channels];
      int b = d.pixels[(static_cast<size_t>(y) * w + x) * channels];
      double diff = a - b;
      sumSq += diff * diff;
      ++n;
    }
  }
  double rmse = std::sqrt(sumSq / n);
  const double limit = quality >= 95 ? 3.0 : 8.0;
  if (rmse > limit)
    reportFailure(__FILE__, __LINE__,
                  "RMSE " + std::to_string(rmse) + " exceeds " +
                      std::to_string(limit));
}

void decodesBothImagesOfAnExport() {
  TiffSpec spec;
  spec.width = 96;
  spec.height = 72;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  std::string in = std::string(ISO21496_TEST_TMPDIR) + "/decode_input.tif";
  writeFile(in, makeTiff(spec, makeHdrPattern(spec.width, spec.height, 5.0f)));

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/decode_output.jpg";
  o.inputTransfer = TransferFunction::Linear;
  EncodeReport report;
  Bytes file = encodeToMemory(o, &report);

  Bytes primary(file.begin(), file.begin() + report.primaryBytes);
  Bytes gain(file.begin() + report.primaryBytes, file.end());

  Decoded p = decode(primary);
  CHECK(p.ok);
  CHECK_EQ(p.width, 96u);
  CHECK_EQ(p.components, 3);
  Decoded g = decode(gain);
  CHECK(g.ok);
  CHECK_EQ(g.width, 48u);
  CHECK_EQ(g.components, 1);

  // The specular corner must be the brightest part of the gain map, and the
  // shadow region must sit near zero gain.
  int corner = g.pixels[(static_cast<size_t>(g.height - 2) * g.width) +
                        (g.width - 2)];
  int shadow = g.pixels[static_cast<size_t>(2) * g.width + 2];
  CHECK(corner > shadow + 40);

  // Whole-file decode: libjpeg reads the primary and ignores the trailer.
  Decoded whole = decode(file);
  CHECK(whole.ok);
  CHECK_EQ(whole.width, 96u);
}

// The promise of a gain map file: a decoder that applies the stored gain to
// the SDR base must land back on the HDR image we started from.
void reconstructedHdrMatchesTheSource() {
  TiffSpec spec;
  spec.width = 256;
  spec.height = 192;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  const float peak = 6.0f;
  auto source = makeHdrPattern(spec.width, spec.height, peak);
  std::string in = std::string(ISO21496_TEST_TMPDIR) + "/recon.tif";
  writeFile(in, makeTiff(spec, source));

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/recon.jpg";
  o.inputTransfer = TransferFunction::Linear;
  o.inputPrimaries = ColorPrimaries::ProPhoto;
  o.outputPrimaries = ColorPrimaries::DisplayP3;
  o.gainMapSubsample = 1;   // exclude resampling error from this measurement
  o.quality = 98;
  o.gainMapQuality = 98;
  EncodeReport report;
  Bytes file = encodeToMemory(o, &report);

  Decoded base = decode(Bytes(file.begin(), file.begin() + report.primaryBytes));
  Decoded gain = decode(Bytes(file.begin() + report.primaryBytes, file.end()));
  CHECK(base.ok);
  CHECK(gain.ok);
  CHECK_EQ(base.width, gain.width);

  const Mat3 toOutput =
      conversionMatrix(ColorPrimaries::ProPhoto, ColorPrimaries::DisplayP3);
  const float minBoost = report.minBoostLog2;
  const float maxBoost = report.maxBoostLog2;

  double sumRel = 0;
  double worstBright = 0;
  size_t n = 0;
  for (uint32_t y = 0; y < base.height; ++y) {
    for (uint32_t x = 0; x < base.width; ++x) {
      const size_t i = static_cast<size_t>(y) * base.width + x;
      // What a compliant decoder computes, per ISO 21496-1.
      float g = gain.pixels[i] / 255.0f;
      float gainLog2 = minBoost + (maxBoost - minBoost) *
                                      std::pow(g, o.gainMapGamma);
      float recon[3];
      for (int c = 0; c < 3; ++c) {
        float sdrLinear = decodeSrgb(base.pixels[i * 3 + c] / 255.0f);
        recon[c] = (sdrLinear + o.offsetSdr) * std::exp2(gainLog2) - o.offsetHdr;
      }
      // What the source says it should be, in the output colour space.
      const float* s = &source[i * 3];
      auto want = toOutput.apply({s[0], s[1], s[2]});

      double wantY = 0.2290 * std::max(0.0, want[0]) +
                     0.6917 * std::max(0.0, want[1]) +
                     0.0793 * std::max(0.0, want[2]);
      double gotY = 0.2290 * std::max(0.0f, recon[0]) +
                    0.6917 * std::max(0.0f, recon[1]) +
                    0.0793 * std::max(0.0f, recon[2]);
      if (wantY < 0.02) continue;  // deep shadows: relative error is meaningless
      double rel = std::fabs(gotY - wantY) / wantY;
      sumRel += rel;
      ++n;
      if (wantY > 2.0) worstBright = std::max(worstBright, rel);
    }
  }
  CHECK(n > 1000);
  const double meanRel = sumRel / n;
  if (meanRel > 0.08)
    reportFailure(__FILE__, __LINE__,
                  "mean reconstruction error " + std::to_string(meanRel) +
                      " exceeds 8%");
  // Specular highlights are the whole point; they must come back accurately.
  if (worstBright > 0.15)
    reportFailure(__FILE__, __LINE__,
                  "worst highlight error " + std::to_string(worstBright) +
                      " exceeds 15%");
}

// Shaping the SDR base changes the fallback rendition, and must leave the HDR
// rendition alone — the gain map is measured against whatever base the shaping
// produces, so the two have to cancel out.
void sdrShapingLeavesTheHdrRenditionAlone() {
  TiffSpec spec;
  spec.width = 192;
  spec.height = 144;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  auto source = makeHdrPattern(spec.width, spec.height, 6.0f);
  std::string in = std::string(ISO21496_TEST_TMPDIR) + "/shaping.tif";
  writeFile(in, makeTiff(spec, source));

  auto renderHdrLuminance = [&](float lift, float contrast,
                                double* midGreySdr) {
    EncoderOptions o;
    o.inputPath = in;
    o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/shaping.jpg";
    o.inputTransfer = TransferFunction::Linear;
    o.inputPrimaries = ColorPrimaries::sRGB;
    o.outputPrimaries = ColorPrimaries::sRGB;
    o.gainMapSubsample = 1;
    o.quality = 98;
    o.gainMapQuality = 98;
    o.sdrLiftEV = lift;
    o.sdrContrast = contrast;
    EncodeReport report;
    Bytes file = encodeToMemory(o, &report);
    Decoded base =
        decode(Bytes(file.begin(), file.begin() + report.primaryBytes));
    Decoded gain =
        decode(Bytes(file.begin() + report.primaryBytes, file.end()));
    CHECK(base.ok);
    CHECK(gain.ok);

    std::vector<double> lum(static_cast<size_t>(base.width) * base.height, 0.0);
    const double wgt[3] = {0.2126, 0.7152, 0.0722};
    double sdrSum = 0;
    size_t sdrCount = 0;
    for (size_t i = 0; i < lum.size(); ++i) {
      float g = gain.pixels[i] / 255.0f;
      float gainLog2 = report.minBoostLog2 +
                       (report.maxBoostLog2 - report.minBoostLog2) *
                           std::pow(g, o.gainMapGamma);
      double y = 0, sdrY = 0;
      for (int c = 0; c < 3; ++c) {
        float sdrLinear = decodeSrgb(base.pixels[i * 3 + c] / 255.0f);
        sdrY += wgt[c] * sdrLinear;
        y += wgt[c] * std::max(0.0f, (sdrLinear + o.offsetSdr) *
                                             std::exp2(gainLog2) -
                                         o.offsetHdr);
      }
      lum[i] = y;
      // The flat 0.18 region of the pattern, for a level reading.
      if (y > 0.15 && y < 0.21) {
        sdrSum += sdrY;
        ++sdrCount;
      }
    }
    if (midGreySdr) *midGreySdr = sdrCount ? sdrSum / sdrCount : 0.0;
    return lum;
  };

  double plainMid = 0, shapedMid = 0;
  auto plain = renderHdrLuminance(0.0f, 1.0f, &plainMid);
  auto shaped = renderHdrLuminance(0.43f, 1.14f, &shapedMid);
  CHECK_EQ(plain.size(), shaped.size());

  double sumEV = 0, maxEV = 0;
  size_t n = 0;
  for (size_t i = 0; i < plain.size(); ++i) {
    if (plain[i] < 0.02 || shaped[i] < 0.02) continue;
    double d = std::fabs(std::log2(shaped[i] / plain[i]));
    sumEV += d;
    maxEV = std::max(maxEV, d);
    ++n;
  }
  CHECK(n > 1000);
  const double meanEV = sumEV / n;
  if (meanEV > 0.03)
    reportFailure(__FILE__, __LINE__,
                  "shaping moved the HDR rendition by " +
                      std::to_string(meanEV) + " EV on average");
  if (maxEV > 0.15)
    reportFailure(__FILE__, __LINE__,
                  "shaping moved one HDR pixel by " + std::to_string(maxEV) +
                      " EV");

  // ...while the SDR base itself does move, which is the entire point.
  if (!(shapedMid > plainMid * 1.08))
    reportFailure(__FILE__, __LINE__,
                  "the lift did not brighten the SDR base (" +
                      std::to_string(plainMid) + " -> " +
                      std::to_string(shapedMid) + ")");
}

void run() {
  decodesOurOutput(64, 64, 3, 95, false, true);
  decodesOurOutput(53, 41, 3, 90, true, true);
  decodesOurOutput(53, 41, 3, 90, true, false);
  decodesOurOutput(37, 19, 1, 95, false, true);
  decodesOurOutput(8, 8, 3, 60, true, true);
  decodesBothImagesOfAnExport();
  reconstructedHdrMatchesTheSource();
  sdrShapingLeavesTheHdrRenditionAlone();
}

}  // namespace

TEST_MAIN(run)
