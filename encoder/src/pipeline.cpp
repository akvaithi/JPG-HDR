#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "icc.h"
#include "threads.h"

namespace iso21496 {
namespace {

// Rec.2020 luminance weights; we do all tone mapping in the output space, so
// the weights are derived from that space's primaries at runtime instead.
std::array<double, 3> luminanceWeights(ColorPrimaries p) {
  Mat3 m = rgbToXyzD65(primariesFor(p));
  return {m.m[1][0], m.m[1][1], m.m[1][2]};
}

float toneMapLuminance(ToneMapOperator op, float l, float lmax) {
  if (l <= 0.0f) return 0.0f;
  switch (op) {
    case ToneMapOperator::Clip:
      return std::min(l, 1.0f);
    case ToneMapOperator::Filmic: {
      // Hable's filmic curve, normalised so that `lmax` maps to exactly 1.
      auto hable = [](float x) {
        constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f,
                        F = 0.30f;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) -
               E / F;
      };
      float denom = hable(lmax);
      if (denom <= 0.0f) return std::min(l, 1.0f);
      return std::min(1.0f, hable(l) / denom);
    }
    case ToneMapOperator::Reinhard:
    default: {
      // Extended Reinhard: identity-ish in the shadows, and l == lmax maps to
      // exactly 1.0 so nothing clips and nothing is left on the table.
      float k = 1.0f + l / (lmax * lmax);
      return std::min(1.0f, l * k / (1.0f + l));
    }
  }
}

// Everything that turns an HDR luminance into the SDR base image, in one
// place: the tone curve, the lift, and the contrast shaping. Used by the pixel
// loop and by the analytic gain-floor sweep, so the two cannot drift apart.
struct SdrShaper {
  ToneMapOperator op = ToneMapOperator::Reinhard;
  float toneCeiling = 16.0f;  // linear; the headroom the tone curve maps to 1.0
  float liftGain = 1.0f;      // 2^sdrLiftEV
  float contrast = 1.0f;
  float pivotScale = 1.0f;    // pivot^(1-contrast), precomputed

  // The tone-mapped, lifted, contrast-shaped luminance, before clamping.
  float shapedLuminance(float lHdr) const {
    float v = toneMapLuminance(op, lHdr, toneCeiling) * liftGain;
    if (std::fabs(contrast - 1.0f) <= 1e-4f) return v;
    if (v <= 0.0f) return 0.0f;
    return std::pow(v, contrast) * pivotScale;
  }

  // The uniform factor applied to linear RGB. Scaling all three channels by
  // the same number is what keeps chromaticity intact: a per-channel power law
  // would spread the channel ratios apart and silently raise saturation.
  float rgbScale(float lHdr) const {
    if (lHdr <= 1e-8f) return 0.0f;
    return shapedLuminance(lHdr) / lHdr;
  }
};

SdrShaper makeShaper(const PipelineOptions& o, float maxBoost) {
  SdrShaper s;
  s.op = o.toneMap;
  s.toneCeiling = std::max(1.0001f, std::pow(2.0f, maxBoost));
  // The lift is an explicit exposure gain on the tone-mapped result, not a
  // reduced tone-curve ceiling. Reducing the ceiling is how a Reinhard-with-
  // white-point curve is usually lifted, but its `1 + L/ceiling^2` term is
  // within a rounding error of 1 through the midtones: measured, shrinking the
  // ceiling by 0.43 EV moved mid grey by 0.001 EV. An exposure gain does what
  // it says — +0.43 EV means +0.43 EV at mid grey — and produces the same
  // end result, since highlights pushed past 1.0 clip in the 8-bit base and
  // are handed back by the gain map, which is measured per pixel against the
  // base as actually written.
  s.liftGain = std::pow(2.0f, std::max(0.0f, o.sdrLiftEV));
  s.contrast = o.sdrContrast;
  constexpr float kPivot = 0.18f;  // linear-light mid grey
  s.pivotScale = std::pow(kPivot, 1.0f - o.sdrContrast);
  return s;
}

// The most negative gain the shaped base can call for. Once the base is
// lifted it is brighter than the HDR image through the midtones, so the gain
// map has to be able to darken as well as brighten; a floor pinned at 0 would
// clamp those pixels and reconstruct them too bright.
//
// Swept analytically rather than measured from the image: the mapping from HDR
// luminance to shaped SDR luminance is one-dimensional, so a dense sweep finds
// the true extreme with no extra pass over the pixels and no dependence on
// whether a downscale happened to catch the right pixel.
float computeGainFloor(const SdrShaper& shaper, const PipelineOptions& o,
                       float maxBoost, bool multiChannel) {
  const float ceiling = std::pow(2.0f, maxBoost);
  float floorLog2 = 0.0f;
  constexpr int kSteps = 4096;
  for (int i = 0; i <= kSteps; ++i) {
    // Log-spaced from deep shadow to the ceiling.
    const float t = static_cast<float>(i) / kSteps;
    const float l = std::pow(2.0f, -20.0f + t * (20.0f + maxBoost));
    if (l > ceiling) break;
    const float k = shaper.rgbScale(l);
    const float sdr = std::min(1.0f, shaper.shapedLuminance(l));
    floorLog2 = std::min(floorLog2,
                         std::log2((l + o.offsetHdr) / (sdr + o.offsetSdr)));
    if (multiChannel && k > 0.0f) {
      // A single channel is most negative right where it reaches 1.0, since
      // below that the shaped value grows faster than the HDR value.
      const float h = std::min(ceiling, 1.0f / k);
      const float s = std::min(1.0f, h * k);
      floorLog2 = std::min(floorLog2,
                           std::log2((h + o.offsetHdr) / (s + o.offsetSdr)));
    }
  }
  // Keep the floor in the territory real captures occupy rather than letting
  // the sweep's extreme define it: an iPhone 17 writes about -0.49 EV, and
  // Lightroom's own export -0.39 to -0.46 EV.
  return std::max(-1.0f, floorLog2);
}

struct Resolved {
  ColorPrimaries primaries;
  TransferFunction transfer;
};

Resolved resolveInputSpace(const TiffReader& tiff, const PipelineOptions& o) {
  Resolved r{ColorPrimaries::ProPhoto, TransferFunction::ROMM};
  IccSummary icc;
  if (!tiff.metadata().iccProfile.empty())
    icc = inspectIccProfile(tiff.metadata().iccProfile);

  if (o.inputPrimaries != ColorPrimaries::Auto) {
    r.primaries = o.inputPrimaries;
  } else if (icc.valid && icc.matched) {
    r.primaries = icc.primaries;
  } else if (tiff.isFloat()) {
    // 32-bit float intermediates from Lightroom are linear ProPhoto.
    r.primaries = ColorPrimaries::ProPhoto;
  }

  if (o.inputTransfer != TransferFunction::Auto) {
    r.transfer = o.inputTransfer;
  } else if (tiff.isFloat()) {
    r.transfer = TransferFunction::Linear;
  } else if (icc.valid && icc.transfer != TransferFunction::Auto) {
    // ProPhoto's ICC advertises gamma 1.8; the real curve has a linear toe.
    r.transfer = (icc.transfer == TransferFunction::Gamma18 &&
                  r.primaries == ColorPrimaries::ProPhoto)
                     ? TransferFunction::ROMM
                     : icc.transfer;
  } else if (r.primaries == ColorPrimaries::ProPhoto) {
    r.transfer = TransferFunction::ROMM;
  } else {
    r.transfer = TransferFunction::sRGB;
  }
  return r;
}

}  // namespace

bool parseToneMap(const std::string& s, ToneMapOperator* out) {
  if (s == "reinhard") *out = ToneMapOperator::Reinhard;
  else if (s == "filmic") *out = ToneMapOperator::Filmic;
  else if (s == "clip" || s == "none") *out = ToneMapOperator::Clip;
  else return false;
  return true;
}

bool parsePeakDetect(const std::string& s, PeakDetect* out) {
  if (s == "softened" || s == "soft") *out = PeakDetect::Softened;
  else if (s == "exact" || s == "peak") *out = PeakDetect::Exact;
  else return false;
  return true;
}

const char* peakDetectName(PeakDetect p) {
  return p == PeakDetect::Exact ? "exact" : "softened";
}

const char* toneMapName(ToneMapOperator t) {
  switch (t) {
    case ToneMapOperator::Filmic: return "filmic";
    case ToneMapOperator::Clip: return "clip";
    default: return "reinhard";
  }
}

PipelineResult runPipeline(const TiffReader& tiff, const PipelineOptions& opts) {
  if (opts.gainMapSubsample != 1 && opts.gainMapSubsample != 2 &&
      opts.gainMapSubsample != 4)
    fail("gain map subsample factor must be 1, 2 or 4");
  if (!(opts.targetHeadroom > 0.0f) || opts.targetHeadroom > 10.0f)
    fail("target headroom must be greater than 0 and at most 10 stops");
  if (!(opts.gainMapGamma > 0.0f)) fail("gain map gamma must be positive");

  const Resolved in = resolveInputSpace(tiff, opts);
  const uint32_t w = tiff.width();
  const uint32_t h = tiff.height();
  const uint32_t inChannels = tiff.channels();
  const int sub = opts.gainMapSubsample;
  const int gainChannels = opts.multiChannelGainMap ? 3 : 1;

  PipelineResult res;
  res.width = w;
  res.height = h;
  res.gainWidth = (w + sub - 1) / sub;
  res.gainHeight = (h + sub - 1) / sub;
  res.gainChannels = gainChannels;
  res.resolvedInputPrimaries = in.primaries;
  res.resolvedInputTransfer = in.transfer;
  res.sdr.assign(static_cast<size_t>(w) * h * 3, 0);

  const Mat3 toOutput = conversionMatrix(in.primaries, opts.outputPrimaries);
  const bool identityMatrix = toOutput.isIdentity();
  const auto lw = luminanceWeights(opts.outputPrimaries);

  // Band height: a multiple of the subsample factor that also lines up with
  // the TIFF's own strip/tile height so nothing is decompressed twice.
  uint32_t band = tiff.suggestedBandRows();
  if (band == 0) band = 64;
  band = ((band + sub - 1) / sub) * sub;
  band = std::max<uint32_t>(band, static_cast<uint32_t>(sub));
  const uint32_t bandCount = (h + band - 1) / band;

  // Pass 1: how much headroom does the content actually use?
  //
  // Every pixel is folded into a box-averaged downscale, and the peak is taken
  // from that. Averaging cuts both ways deliberately: a lone hot pixel or a
  // speck of sensor noise cannot define the headroom for the whole frame, and
  // — unlike sampling a grid of pixels, which this replaces — a small specular
  // highlight can never be skipped over entirely, because it always raises the
  // block that contains it.
  float maxBoost = opts.targetHeadroom;
  float measured = 0.0f;
  float truePeak = 0.0f;
  {
    const uint32_t longEdge = std::max(w, h);
    // Roughly a 2048-pixel long edge, and always at least 2x2 on anything
    // bigger than a thumbnail, so there is some averaging even when small.
    uint32_t blockSize = (longEdge + 2047u) / 2048u;
    if (longEdge > 512) blockSize = std::max<uint32_t>(blockSize, 2);
    blockSize = std::max<uint32_t>(blockSize, 1);
    if (opts.peakDetect == PeakDetect::Exact) blockSize = 1;

    const uint32_t blocksX = (w + blockSize - 1) / blockSize;

    // Output-space luminance as a function of the *input* primaries, so the
    // measurement pass never has to build a converted RGB triple.
    double inputLumWeights[3];
    for (int c = 0; c < 3; ++c) {
      inputLumWeights[c] = identityMatrix
                               ? lw[c]
                               : lw[0] * toOutput.m[0][c] +
                                     lw[1] * toOutput.m[1][c] +
                                     lw[2] * toOutput.m[2][c];
    }
    const double inputLumWeightSum =
        inputLumWeights[0] + inputLumWeights[1] + inputLumWeights[2];

    // Read in whole strips or tiles, not one block row at a time: readRows
    // decodes every segment its range touches, so a 4-row read against a
    // 32-row strip layout would decode each strip eight times over.
    uint32_t readRows = tiff.suggestedBandRows();
    if (readRows == 0) readRows = blockSize * 16;
    readRows = ((readRows + blockSize - 1) / blockSize) * blockSize;
    readRows = std::max(readRows, blockSize);
    const uint32_t readBands = (h + readRows - 1) / readRows;

    std::vector<float> blockPeak(readBands, 0.0f);
    std::vector<float> pixelPeak(readBands, 0.0f);

    parallelFor(readBands, opts.threads, [&](size_t bi) {
      const uint32_t y0 = static_cast<uint32_t>(bi) * readRows;
      const uint32_t rows = std::min(readRows, h - y0);
      std::vector<float> src(static_cast<size_t>(rows) * w * inChannels);
      tiff.readRows(y0, rows, src.data());

      const uint32_t blockRowsHere = (rows + blockSize - 1) / blockSize;
      std::vector<double> sums(static_cast<size_t>(blockRowsHere) * blocksX, 0.0);
      std::vector<uint32_t> counts(static_cast<size_t>(blockRowsHere) * blocksX, 0);
      float localPixelPeak = 0.0f;

      for (uint32_t r = 0; r < rows; ++r) {
        const float* srow = src.data() + static_cast<size_t>(r) * w * inChannels;
        double* sumRow = sums.data() + static_cast<size_t>(r / blockSize) * blocksX;
        uint32_t* cntRow =
            counts.data() + static_cast<size_t>(r / blockSize) * blocksX;
        for (uint32_t x = 0; x < w; ++x) {
          // Only luminance is needed here, so the primaries matrix is folded
          // into the weights: one dot product instead of a 3x3 multiply.
          float l = 0.0f;
          if (inChannels == 1) {
            l = static_cast<float>(inputLumWeightSum) *
                decodeTransfer(in.transfer, srow[x], opts.pqDiffuseWhiteNits);
          } else {
            const float* p = srow + static_cast<size_t>(x) * inChannels;
            for (int c = 0; c < 3; ++c)
              l += static_cast<float>(inputLumWeights[c]) *
                   decodeTransfer(in.transfer, p[c], opts.pqDiffuseWhiteNits);
          }
          if (l < 0.0f) l = 0.0f;
          sumRow[x / blockSize] += l;
          ++cntRow[x / blockSize];
          localPixelPeak = std::max(localPixelPeak, l);
        }
      }

      float bandPeak = 0.0f;
      for (size_t b = 0; b < sums.size(); ++b) {
        if (counts[b] == 0) continue;
        bandPeak = std::max(bandPeak, static_cast<float>(sums[b] / counts[b]));
      }
      blockPeak[bi] = bandPeak;
      pixelPeak[bi] = localPixelPeak;
    });

    float peak = 0.0f;
    for (uint32_t bi = 0; bi < readBands; ++bi) {
      peak = std::max(peak, blockPeak[bi]);
      truePeak = std::max(truePeak, pixelPeak[bi]);
    }
    measured = peak > 1.0f ? std::log2(peak) : 0.0f;
    res.measuredHeadroom = measured;
    res.truePeakHeadroom = truePeak > 1.0f ? std::log2(truePeak) : 0.0f;
    if (opts.autoMaxBoost) {
      // Leave a sixth of a stop of slack for interpolation and rounding.
      maxBoost = std::min(opts.targetHeadroom, measured + 1.0f / 6.0f);
      maxBoost = std::max(maxBoost, 0.0f);
    }
  }
  if (maxBoost <= 0.0f) maxBoost = 1e-4f;  // keep the encoding well defined

  // The HDR clamp and the gain map share one ceiling, so every highlight the
  // base image gives up is one the gain map can hand back.
  const float lmax = std::pow(2.0f, maxBoost);
  const SdrShaper shaper = makeShaper(opts, maxBoost);
  const float minBoost =
      computeGainFloor(shaper, opts, maxBoost, opts.multiChannelGainMap);

  res.declaredHeadroom = maxBoost;
  for (int c = 0; c < 3; ++c) {
    res.minBoostLog2[c] = minBoost;
    res.maxBoostLog2[c] = maxBoost;
  }

  res.gain.assign(static_cast<size_t>(res.gainWidth) * res.gainHeight *
                      gainChannels,
                  0);

  const float invGamma = 1.0f / opts.gainMapGamma;
  const float boostRange = maxBoost - minBoost;

  parallelFor(bandCount, opts.threads, [&](size_t bi) {
    const uint32_t y0 = static_cast<uint32_t>(bi) * band;
    const uint32_t rows = std::min(band, h - y0);
    std::vector<float> src(static_cast<size_t>(rows) * w * inChannels);
    tiff.readRows(y0, rows, src.data());

    // Gain accumulators for the (partial) gain-map rows this band covers.
    const uint32_t gy0 = y0 / sub;
    const uint32_t gRows = (rows + sub - 1) / sub;
    std::vector<float> accum(static_cast<size_t>(gRows) * res.gainWidth *
                                 gainChannels,
                             0.0f);
    std::vector<uint16_t> counts(static_cast<size_t>(gRows) * res.gainWidth, 0);

    for (uint32_t r = 0; r < rows; ++r) {
      const float* srow = src.data() + static_cast<size_t>(r) * w * inChannels;
      uint8_t* drow = res.sdr.data() + (static_cast<size_t>(y0 + r) * w) * 3;
      const uint32_t gr = r / sub;
      for (uint32_t x = 0; x < w; ++x) {
        float lin[3];
        for (int c = 0; c < 3; ++c) {
          float v = srow[static_cast<size_t>(x) * inChannels +
                         (inChannels == 1 ? 0 : c)];
          lin[c] = decodeTransfer(in.transfer, v, opts.pqDiffuseWhiteNits);
        }
        float hdr[3];
        if (identityMatrix) {
          hdr[0] = lin[0]; hdr[1] = lin[1]; hdr[2] = lin[2];
        } else {
          auto v = toOutput.apply({lin[0], lin[1], lin[2]});
          hdr[0] = static_cast<float>(v[0]);
          hdr[1] = static_cast<float>(v[1]);
          hdr[2] = static_cast<float>(v[2]);
        }
        // Out-of-gamut negatives are clipped; values above the target
        // headroom are held at the ceiling the metadata promises.
        for (int c = 0; c < 3; ++c)
          hdr[c] = std::min(lmax, std::max(0.0f, hdr[c]));

        const float lHdr = std::max(0.0f, static_cast<float>(
                                              lw[0] * hdr[0] + lw[1] * hdr[1] +
                                              lw[2] * hdr[2]));
        // Tone curve, lift and contrast collapse into one scale on RGB, so
        // the base image's chromaticity matches the HDR image's exactly.
        const float scale = shaper.rgbScale(lHdr);

        float sdr[3];
        for (int c = 0; c < 3; ++c)
          sdr[c] = std::min(1.0f, std::max(0.0f, hdr[c] * scale));

        uint8_t* out = drow + static_cast<size_t>(x) * 3;
        for (int c = 0; c < 3; ++c)
          out[c] = static_cast<uint8_t>(
              std::lround(encodeSrgb(sdr[c]) * 255.0f));

        const uint32_t gx = x / sub;
        float* acc = accum.data() +
                     (static_cast<size_t>(gr) * res.gainWidth + gx) *
                         gainChannels;
        if (gainChannels == 1) {
          // Single achromatic channel: the luminance ratio the decoder needs,
          // measured against the base image as actually written — shaping and
          // per-channel clipping included.
          const float lSdr = std::min(1.0f, std::max(0.0f,
              static_cast<float>(lw[0] * sdr[0] + lw[1] * sdr[1] +
                                 lw[2] * sdr[2])));
          float g = std::log2((lHdr + opts.offsetHdr) /
                              (lSdr + opts.offsetSdr));
          acc[0] += g;
        } else {
          for (int c = 0; c < 3; ++c)
            acc[c] += std::log2((hdr[c] + opts.offsetHdr) /
                                (sdr[c] + opts.offsetSdr));
        }
        ++counts[static_cast<size_t>(gr) * res.gainWidth + gx];
      }
    }

    // Box-average, normalise into [0,1], then apply the encoding gamma.
    for (uint32_t gr = 0; gr < gRows; ++gr) {
      for (uint32_t gx = 0; gx < res.gainWidth; ++gx) {
        const size_t idx = static_cast<size_t>(gr) * res.gainWidth + gx;
        const float n = counts[idx] ? static_cast<float>(counts[idx]) : 1.0f;
        uint8_t* dst = res.gain.data() +
                       (static_cast<size_t>(gy0 + gr) * res.gainWidth + gx) *
                           gainChannels;
        for (int c = 0; c < gainChannels; ++c) {
          float g = accum[idx * gainChannels + c] / n;
          float norm = boostRange > 0.0f ? (g - minBoost) / boostRange : 0.0f;
          norm = std::min(1.0f, std::max(0.0f, norm));
          dst[c] = static_cast<uint8_t>(
              std::lround(std::pow(norm, invGamma) * 255.0f));
        }
      }
    }
  });

  return res;
}

}  // namespace iso21496
